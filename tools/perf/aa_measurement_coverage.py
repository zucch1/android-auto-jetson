# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared, stdlib-only active-window evidence contract.

The frozen records are the collector/export integration boundary. JSON is parsed
only here; no collector or reporter import is permitted.
"""
# allow: SIZE_OK -- Frozen single-file API is bundled onto the target; private
# sections keep records, JSON parsing, sealing and reduction independently named.
from __future__ import annotations

import math
from collections.abc import Mapping
from dataclasses import asdict, dataclass, fields
from types import MappingProxyType
from typing import Final, Literal

POLICY: Final = "active-window-coverage/1"
QUANTILE_POLICY: Final = "nearest-rank-last-4096-eligible-processing-order"
LIMIT: Final = 4096
HORIZON_NS: Final = 10_000_000_000
Stream = Literal["ipc_consumption", "ipc_matches", "decode"]
STREAMS: Final[tuple[Stream, ...]] = ("ipc_consumption", "ipc_matches", "decode")


@dataclass(frozen=True, slots=True)
class CoverageError(ValueError):
    detail: str

    def __str__(self) -> str:
        return self.detail


@dataclass(frozen=True, slots=True)
class Window:
    start_ns: int
    end_ns: int


@dataclass(frozen=True, slots=True)
class Contribution:
    stream: Stream
    begin_ns: int
    end_ns: int
    bytes: int


@dataclass(frozen=True, slots=True)
class StreamBlock:
    count: int
    first_ns: int | None
    last_ns: int | None
    max_internal_gap_ns: int
    begin_ns: int | None
    bytes: int


@dataclass(frozen=True, slots=True)
class CoverageBlock:
    start_ns: int
    end_ns: int
    ipc_consumption: StreamBlock
    ipc_matches: StreamBlock
    decode: StreamBlock


@dataclass(frozen=True, slots=True)
class ThermalZone:
    zone: str
    temp_c: float | None


@dataclass(frozen=True, slots=True)
class Sample:
    t_ns: int
    receiver_rss_mb: float | None
    receiver_cpu_ticks: int | None
    thermal_zones: tuple[ThermalZone, ...]
    projection_active: bool
    video_units: int | None
    audio_chunks: int | None
    video_presentations: int | None
    audio_writes: int | None
    av_pairs: int | None


@dataclass(frozen=True, slots=True)
class VideoEvidence:
    blocks: tuple[CoverageBlock, ...]
    ipc_latency_ns: tuple[int, ...]
    decode_latency_ns: tuple[int, ...]


@dataclass(frozen=True, slots=True)
class CaptureEvidence:
    monotonic_origin_ns: int
    cpu_ticks_per_second: int
    samples: tuple[Sample, ...]
    video_evidence: VideoEvidence


@dataclass(frozen=True, slots=True)
class Timeline:
    count: int
    first_ns: int | None
    last_ns: int | None
    max_gap_ns: int


@dataclass(frozen=True, slots=True)
class Observation:
    expected: int
    valid: int
    missing: int
    first_ns: int | None
    last_ns: int | None
    max_gap_ns: int


@dataclass(frozen=True, slots=True)
class CpuCoverage:
    expected: int
    valid: int
    missing: int
    first_ns: int | None
    last_ns: int | None
    max_gap_ns: int
    baseline_valid: bool
    measured_interval_ns: int


@dataclass(frozen=True, slots=True)
class EndpointCoverage:
    count: int
    first_ns: int | None
    last_ns: int | None
    max_gap_ns: int
    begin_ns: int | None
    bytes: int


@dataclass(frozen=True, slots=True)
class QuantileScope:
    policy: str
    limit: int
    ipc_retained: int
    decode_retained: int


@dataclass(frozen=True, slots=True)
class Coverage:
    policy: str
    window: Window | None
    sampling: Timeline
    rss: Observation
    cpu: CpuCoverage
    thermal: Observation
    ipc_consumption: EndpointCoverage
    ipc_matches: EndpointCoverage
    decode: EndpointCoverage
    quantiles: QuantileScope


@dataclass(frozen=True, slots=True)
class ThermalState:
    peak_c: float
    zones: tuple[ThermalZone, ...]


@dataclass(frozen=True, slots=True)
class Reduction:
    measurement_coverage: Coverage
    metrics: Mapping[str, int | float | None]
    thermal_state: ThermalState | None


def _stream_block(items: list[Contribution]) -> StreamBlock:
    ordered = sorted(items, key=lambda item: item.end_ns)
    times = [item.end_ns for item in ordered]
    return StreamBlock(len(items), times[0] if times else None,
                       times[-1] if times else None,
                       max((b - a for a, b in zip(times, times[1:])), default=0),
                       min((item.begin_ns for item in items), default=None),
                       sum(item.bytes for item in items))


class CoverageLedger:
    """Mutable event-time accumulator; sealing releases bounded open evidence."""

    def __init__(self, start_ns: int) -> None:
        _integer(start_ns)
        self._start = start_ns
        self._boundaries = [start_ns]
        self._sealed_end = start_ns - 1
        self._open: dict[Stream, list[Contribution]] = {stream: [] for stream in STREAMS}
        self._blocks: list[CoverageBlock] = []
        self._issues: set[str] = set()
        self._finished = False

    def add(self, contribution: Contribution) -> bool:
        if self._finished:
            raise CoverageError("ledger already finished")
        if contribution.stream not in STREAMS:
            raise CoverageError("unknown contribution stream")
        _integer(contribution.begin_ns)
        _integer(contribution.end_ns)
        if contribution.end_ns < self._start or contribution.end_ns <= self._sealed_end:
            return False
        if not self._start <= contribution.begin_ns <= contribution.end_ns:
            return False
        _integer(contribution.bytes)
        if ((contribution.stream == "ipc_consumption" and contribution.bytes < 1)
                or (contribution.stream != "ipc_consumption" and contribution.bytes != 0)):
            raise CoverageError("invalid contribution bytes")
        items = self._open[contribution.stream]
        if len(items) >= LIMIT:
            self._issues.add("coverage_capacity")
            return False
        items.append(contribution)
        return True

    def advance(self, sample_ns: int) -> None:
        _integer(sample_ns)
        if self._finished or sample_ns <= self._boundaries[-1]:
            raise CoverageError("sample boundaries must strictly increase")
        self._boundaries.append(sample_ns)
        self._seal(sample_ns - HORIZON_NS)

    def _seal(self, through_ns: int) -> None:
        while len(self._blocks) + 1 < len(self._boundaries):
            index = len(self._blocks)
            start, end = self._boundaries[index:index + 2]
            if end > through_ns:
                break
            summaries: list[StreamBlock] = []
            for stream in STREAMS:
                items = self._open[stream]
                summaries.append(_stream_block([item for item in items if item.end_ns <= end]))
                self._open[stream] = [item for item in items if item.end_ns > end]
            self._blocks.append(CoverageBlock(start, end, *summaries))
            self._sealed_end = end

    def finish(self, end_ns: int) -> tuple[CoverageBlock, ...]:
        if self._finished or end_ns != self._boundaries[-1]:
            raise CoverageError("finish requires the final registered sample exactly once")
        self._seal(end_ns)
        self._finished = True
        return tuple(self._blocks)

    @property
    def issues(self) -> tuple[str, ...]:
        return tuple(sorted(self._issues))


def _integer(raw: object) -> int:
    if isinstance(raw, bool) or not isinstance(raw, int) or raw < 0:
        raise CoverageError("expected nonnegative integer, never boolean")
    return raw


def _nullable_integer(raw: object) -> int | None:
    return None if raw is None else _integer(raw)


def _number(raw: object) -> float | None:
    if raw is None:
        return None
    if isinstance(raw, bool) or not isinstance(raw, (int, float)):
        raise CoverageError("expected finite number or null")
    try:
        value = float(raw)
    except OverflowError as exc:
        raise CoverageError("number overflow") from exc
    if not math.isfinite(value):
        raise CoverageError("nonfinite number")
    return value


def _object(raw: object) -> dict[str, object]:
    if not isinstance(raw, dict) or any(not isinstance(key, str) for key in raw):
        raise CoverageError("expected JSON object")
    return {str(key): value for key, value in raw.items()}


def _exact(raw: object, names: tuple[str, ...]) -> dict[str, object]:
    value = _object(raw)
    if set(value) != set(names):
        raise CoverageError("wrong object fields: expected " + ",".join(names))
    return value


def _array(raw: object) -> list[object]:
    if not isinstance(raw, list):
        raise CoverageError("expected JSON array")
    return list(raw)


def _boolean(raw: object) -> bool:
    if not isinstance(raw, bool):
        raise CoverageError("expected boolean")
    return raw


def _sample(raw: object) -> Sample:
    obj = _exact(raw, tuple(field.name for field in fields(Sample)))
    zones: list[ThermalZone] = []
    for item in _array(obj["thermal_zones"]):
        zone = _exact(item, ("zone", "temp_c"))
        name = zone["zone"]
        if not isinstance(name, str) or not name or any(z.zone == name for z in zones):
            raise CoverageError("thermal zone identities must be unique nonempty strings")
        zones.append(ThermalZone(name, _number(zone["temp_c"])))
    rss = _number(obj["receiver_rss_mb"])
    if rss is not None and rss < 0:
        raise CoverageError("negative RSS")
    return Sample(_integer(obj["t_ns"]), rss, _nullable_integer(obj["receiver_cpu_ticks"]),
                  tuple(zones), _boolean(obj["projection_active"]),
                  *(_nullable_integer(obj[key]) for key in
                    ("video_units", "audio_chunks", "video_presentations", "audio_writes", "av_pairs")))


def _raw_stream(raw: object) -> StreamBlock:
    obj = _exact(raw, tuple(field.name for field in fields(StreamBlock)))
    return StreamBlock(_integer(obj["count"]), _nullable_integer(obj["first_ns"]),
                       _nullable_integer(obj["last_ns"]), _integer(obj["max_internal_gap_ns"]),
                       _nullable_integer(obj["begin_ns"]), _integer(obj["bytes"]))


def parse_capture(raw: object) -> CaptureEvidence:
    obj = _object(raw)
    required = ("monotonic_origin_ns", "cpu_ticks_per_second", "samples", "video_evidence")
    if any(key not in obj for key in required):
        raise CoverageError("missing v2 capture evidence")
    clock = _integer(obj["cpu_ticks_per_second"])
    if clock == 0:
        raise CoverageError("CPU tick frequency must be positive")
    samples = tuple(_sample(item) for item in _array(obj["samples"]))
    if any(b.t_ns <= a.t_ns for a, b in zip(samples, samples[1:])):
        raise CoverageError("sample timestamps must strictly increase")
    video = _exact(obj["video_evidence"], ("blocks", "ipc_latency_ns", "decode_latency_ns"))
    blocks: list[CoverageBlock] = []
    for item in _array(video["blocks"]):
        block = _exact(item, ("start_ns", "end_ns", *STREAMS))
        blocks.append(CoverageBlock(_integer(block["start_ns"]), _integer(block["end_ns"]),
                                    *(_raw_stream(block[stream]) for stream in STREAMS)))
    ipc = tuple(_integer(item) for item in _array(video["ipc_latency_ns"]))
    decode = tuple(_integer(item) for item in _array(video["decode_latency_ns"]))
    if (len(ipc) > LIMIT or len(decode) > LIMIT or any(d >= HORIZON_NS for d in ipc)
            or any(d >= 5_000_000_000 for d in decode)):
        raise CoverageError("invalid quantile ring")
    return CaptureEvidence(_integer(obj["monotonic_origin_ns"]), clock, samples,
                           VideoEvidence(tuple(blocks), ipc, decode))


def _point_bounds(point: Timeline, window: Window | None) -> None:
    duration = window.end_ns - window.start_ns if window else 0
    if point.count == 0:
        if point.first_ns is not None or point.last_ns is not None or point.max_gap_ns != duration:
            raise CoverageError("empty point stream must have null bounds and whole-window gap")
        return
    if window is None or point.first_ns is None or point.last_ns is None:
        raise CoverageError("nonempty point stream requires window and bounds")
    first, last = point.first_ns, point.last_ns
    if not window.start_ns <= first <= last <= window.end_ns:
        raise CoverageError("point bounds outside window")
    edge = max(first - window.start_ns, window.end_ns - last)
    if point.count == 1:
        if first != last or point.max_gap_ns != edge:
            raise CoverageError("single point bounds or gap inconsistent")
    elif not max(edge, (last - first + point.count - 2) // (point.count - 1)) <= point.max_gap_ns <= max(edge, last - first):
        raise CoverageError("point gap contradicts count and bounds")


def _observation(raw: object) -> Observation:
    obj = _exact(raw, tuple(field.name for field in fields(Observation)))
    return Observation(_integer(obj["expected"]), _integer(obj["valid"]),
                       _integer(obj["missing"]), _nullable_integer(obj["first_ns"]),
                       _nullable_integer(obj["last_ns"]), _integer(obj["max_gap_ns"]))


def _endpoint(raw: object) -> EndpointCoverage:
    obj = _exact(raw, tuple(field.name for field in fields(EndpointCoverage)))
    return EndpointCoverage(_integer(obj["count"]), _nullable_integer(obj["first_ns"]),
                            _nullable_integer(obj["last_ns"]), _integer(obj["max_gap_ns"]),
                            _nullable_integer(obj["begin_ns"]), _integer(obj["bytes"]))


def parse_coverage(raw: object) -> Coverage:
    obj = _exact(raw, tuple(field.name for field in fields(Coverage)))
    window = None
    if obj["window"] is not None:
        bounds = _exact(obj["window"], ("start_ns", "end_ns"))
        window = Window(_integer(bounds["start_ns"]), _integer(bounds["end_ns"]))
        if window.end_ns < window.start_ns:
            raise CoverageError("reversed window")
    sampling = _exact(obj["sampling"], tuple(field.name for field in fields(Timeline)))
    timeline = Timeline(_integer(sampling["count"]), _nullable_integer(sampling["first_ns"]),
                        _nullable_integer(sampling["last_ns"]), _integer(sampling["max_gap_ns"]))
    rss, thermal = _observation(obj["rss"]), _observation(obj["thermal"])
    cpu = _exact(obj["cpu"], tuple(field.name for field in fields(CpuCoverage)))
    cpu_counts = _observation({key: cpu[key] for key in (field.name for field in fields(Observation))})
    cpu_coverage = CpuCoverage(cpu_counts.expected, cpu_counts.valid, cpu_counts.missing,
                               cpu_counts.first_ns, cpu_counts.last_ns, cpu_counts.max_gap_ns,
                               _boolean(cpu["baseline_valid"]), _integer(cpu["measured_interval_ns"]))
    quantiles = _exact(obj["quantiles"], tuple(field.name for field in fields(QuantileScope)))
    if obj["policy"] != POLICY or quantiles["policy"] != QUANTILE_POLICY or _integer(quantiles["limit"]) != LIMIT:
        raise CoverageError("unknown coverage or quantile policy")
    endpoints = tuple(_endpoint(obj[stream]) for stream in STREAMS)
    result = Coverage(POLICY, window, timeline, rss, cpu_coverage, thermal, *endpoints,
                      QuantileScope(QUANTILE_POLICY, LIMIT, _integer(quantiles["ipc_retained"]),
                                    _integer(quantiles["decode_retained"])))
    _validate_coverage(result)
    return result


def _ipc_subset(consumed: StreamBlock | EndpointCoverage,
                matched: StreamBlock | EndpointCoverage) -> None:
    if matched.count > consumed.count:
        raise CoverageError("IPC matches exceed consumed count")
    if matched.count:
        if (consumed.first_ns is None or consumed.last_ns is None
                or matched.first_ns is None or matched.last_ns is None
                or not consumed.first_ns <= matched.first_ns <= matched.last_ns <= consumed.last_ns):
            raise CoverageError("IPC matched receives outside consumed bounds")


def _validate_coverage(value: Coverage) -> None:
    window, sampling, cpu = value.window, value.sampling, value.cpu
    duration = window.end_ns - window.start_ns if window else 0
    _point_bounds(sampling, window)
    if window and (sampling.count == 0 or sampling.first_ns != window.start_ns or sampling.last_ns != window.end_ns):
        raise CoverageError("sampling must span the active window")
    for observation in (value.rss, value.thermal, cpu):
        expected = max(sampling.count - 1, 0) if observation is cpu else sampling.count
        if observation.expected != expected or observation.valid > expected or observation.missing != expected - observation.valid:
            raise CoverageError("inconsistent observation counts")
    for observation in (value.rss, value.thermal):
        point = Timeline(observation.valid, observation.first_ns, observation.last_ns, observation.max_gap_ns)
        _point_bounds(point, window)
        if observation.missing == 0 and point != sampling:
            raise CoverageError("complete observation must equal sampling timeline")
    if cpu.measured_interval_ns > duration:
        raise CoverageError("CPU duration exceeds window")
    if cpu.valid == 0:
        if (cpu.measured_interval_ns != 0 or cpu.first_ns is not None or cpu.last_ns is not None
                or cpu.max_gap_ns != duration):
            raise CoverageError("empty CPU intervals inconsistent")
    else:
        if window is None or cpu.first_ns is None or cpu.last_ns is None:
            raise CoverageError("CPU intervals require window and bounds")
        if not window.start_ns <= cpu.first_ns < cpu.last_ns <= window.end_ns:
            raise CoverageError("CPU bounds outside window")
        if cpu.first_ns == window.start_ns and not cpu.baseline_valid:
            raise CoverageError("first CPU interval contradicts missing baseline")
        span = cpu.last_ns - cpu.first_ns
        if not cpu.valid <= cpu.measured_interval_ns <= span:
            raise CoverageError("CPU measured duration contradicts count or span")
        internal = span - cpu.measured_interval_ns
        edge = max(cpu.first_ns - window.start_ns, window.end_ns - cpu.last_ns)
        lower = max(edge, (internal + max(1, cpu.missing) - 1) // max(1, cpu.missing))
        if not lower <= cpu.max_gap_ns <= max(edge, internal):
            raise CoverageError("CPU uncovered gap inconsistent")
        if cpu.missing == 0 and (not cpu.baseline_valid or cpu.measured_interval_ns != duration or cpu.max_gap_ns != 0):
            raise CoverageError("complete CPU interval assertions inconsistent")
    if window is None and cpu.baseline_valid:
        raise CoverageError("CPU baseline without window")
    for stream in STREAMS:
        endpoint = getattr(value, stream)
        _point_bounds(Timeline(endpoint.count, endpoint.first_ns, endpoint.last_ns, endpoint.max_gap_ns), window)
        if endpoint.count == 0:
            if endpoint.begin_ns is not None or endpoint.bytes != 0:
                raise CoverageError("empty endpoint begin/bytes inconsistent")
        elif (window is None or endpoint.begin_ns is None or endpoint.first_ns is None
              or not window.start_ns <= endpoint.begin_ns <= endpoint.first_ns):
            raise CoverageError("endpoint begin outside window")
        if ((stream == "ipc_consumption" and endpoint.bytes < endpoint.count)
                or (stream != "ipc_consumption" and endpoint.bytes != 0)):
            raise CoverageError("endpoint byte count inconsistent")
    _ipc_subset(value.ipc_consumption, value.ipc_matches)
    if (value.quantiles.ipc_retained != min(LIMIT, value.ipc_matches.count)
            or value.quantiles.decode_retained != min(LIMIT, value.decode.count)):
        raise CoverageError("retained quantile counts inconsistent")


def coverage_to_json(coverage: Coverage) -> dict[str, object]:
    return asdict(coverage)


def coverage_gaps(coverage: Coverage, active_seconds: float | None) -> tuple[str, ...]:
    _validate_coverage(coverage)
    gaps: list[str] = []
    window, sampling, cpu = coverage.window, coverage.sampling, coverage.cpu
    duration = window.end_ns - window.start_ns if window else 0
    if sampling.count < 2 or sampling.max_gap_ns > HORIZON_NS:
        gaps.append("coverage:sampling")
    for name, observation in (("rss", coverage.rss), ("thermal", coverage.thermal)):
        if observation.valid == 0 or observation.missing or observation.max_gap_ns > HORIZON_NS:
            gaps.append("coverage:" + name)
    if (not cpu.baseline_valid or sampling.count < 2 or cpu.missing
            or cpu.measured_interval_ns != duration or cpu.max_gap_ns != 0
            or window is None or cpu.first_ns != window.start_ns or cpu.last_ns != window.end_ns):
        gaps.append("coverage:cpu")
    for stream in STREAMS:
        endpoint = getattr(coverage, stream)
        if endpoint.count == 0 or endpoint.max_gap_ns > HORIZON_NS:
            gaps.append("coverage:" + stream)
    seconds = _number(active_seconds)
    if window is None or seconds is None or abs(seconds * 1e9 - duration) > 1:
        gaps.append("coverage:window")
    order = ("sampling", "rss", "cpu", "thermal", *STREAMS, "window")
    return tuple("coverage:" + name for name in order if "coverage:" + name in gaps)


def _timeline(times: list[int], window: Window | None) -> Timeline:
    if window is None:
        return Timeline(0, None, None, 0)
    if not times:
        return Timeline(0, None, None, window.end_ns - window.start_ns)
    return Timeline(len(times), times[0], times[-1],
                    max(times[0] - window.start_ns, window.end_ns - times[-1],
                        max((b - a for a, b in zip(times, times[1:])), default=0)))


def _observed(timeline: Timeline, expected: int) -> Observation:
    return Observation(expected, timeline.count, expected - timeline.count,
                       timeline.first_ns, timeline.last_ns, timeline.max_gap_ns)


def _merge_blocks(blocks: tuple[CoverageBlock, ...], stream: Stream,
                  window: Window | None) -> EndpointCoverage:
    count, total_bytes, gap = 0, 0, 0
    first: int | None = None
    last: int | None = None
    begin: int | None = None
    for block in blocks:
        summary: StreamBlock = getattr(block, stream)
        if block.end_ns <= block.start_ns:
            raise CoverageError("block duration must be positive")
        point = Timeline(summary.count, summary.first_ns, summary.last_ns,
                         summary.max_internal_gap_ns)
        if point.count == 0:
            if (point.first_ns is not None or point.last_ns is not None or point.max_gap_ns != 0
                    or summary.begin_ns is not None or summary.bytes != 0):
                raise CoverageError("empty raw stream summary inconsistent")
            continue
        if window is None or point.first_ns is None or point.last_ns is None or summary.begin_ns is None:
            raise CoverageError("nonempty block requires bounds")
        lower = block.start_ns if block.start_ns == window.start_ns else block.start_ns + 1
        if not lower <= point.first_ns <= point.last_ns <= block.end_ns:
            raise CoverageError("endpoint outside its sample interval")
        if not window.start_ns <= summary.begin_ns <= point.first_ns:
            raise CoverageError("raw stream begin outside active window")
        _point_bounds(point, Window(point.first_ns, point.last_ns))
        if ((stream == "ipc_consumption" and summary.bytes < summary.count)
                or (stream != "ipc_consumption" and summary.bytes != 0)):
            raise CoverageError("raw stream bytes inconsistent")
        gap = max(gap, summary.max_internal_gap_ns,
                  point.first_ns - last if last is not None else 0)
        first = point.first_ns if first is None else first
        last = point.last_ns
        begin = summary.begin_ns if begin is None else min(begin, summary.begin_ns)
        count += summary.count
        total_bytes += summary.bytes
    if window:
        gap = (max(gap, first - window.start_ns, window.end_ns - last)
               if first is not None and last is not None else window.end_ns - window.start_ns)
    return EndpointCoverage(count, first, last, gap, begin, total_bytes)


def _latency_bounds(summaries: tuple[StreamBlock, ...], ring: tuple[int, ...]) -> None:
    spans = sorted(((summary.last_ns - summary.begin_ns, summary.count)
                    for summary in summaries
                    if summary.count and summary.last_ns is not None and summary.begin_ns is not None),
                   reverse=True)
    index, available = 0, 0
    for rank, delta in enumerate(sorted(ring, reverse=True)):
        while index < len(spans) and spans[index][0] >= delta:
            available += spans[index][1]
            index += 1
        if available <= rank:
            raise CoverageError("latency ring exceeds contributing block bounds/counts")


def reduce_capture(evidence: CaptureEvidence, window: Window | None) -> Reduction:
    if window and (window.start_ns < 0 or window.end_ns < window.start_ns):
        raise CoverageError("invalid active window")
    active = tuple(sample for sample in evidence.samples if window
                   and window.start_ns <= sample.t_ns <= window.end_ns)
    times = [sample.t_ns for sample in active]
    sampling = _timeline(times, window)
    if window and (not times or times[0] != window.start_ns or times[-1] != window.end_ns):
        raise CoverageError("active window must use sample boundaries")
    blocks = evidence.video_evidence.blocks
    if len(blocks) != max(0, len(active) - 1):
        raise CoverageError("one block required for every active sample interval")
    if any(block.start_ns != a.t_ns or block.end_ns != b.t_ns
           for block, a, b in zip(blocks, active, active[1:])):
        raise CoverageError("blocks do not exactly partition active samples")
    endpoints = tuple(_merge_blocks(blocks, stream, window) for stream in STREAMS)
    for block in blocks:
        _ipc_subset(block.ipc_consumption, block.ipc_matches)
    _latency_bounds(tuple(block.ipc_matches for block in blocks), evidence.video_evidence.ipc_latency_ns)
    _latency_bounds(tuple(block.decode for block in blocks), evidence.video_evidence.decode_latency_ns)
    rss_values = [sample.receiver_rss_mb for sample in active if sample.receiver_rss_mb is not None]
    rss = _observed(_timeline([sample.t_ns for sample in active if sample.receiver_rss_mb is not None], window), len(active))
    inventory = {zone.zone for zone in active[0].thermal_zones} if active else set()
    thermal_times = [sample.t_ns for sample in active if inventory
                     and {zone.zone for zone in sample.thermal_zones} == inventory
                     and all(zone.temp_c is not None for zone in sample.thermal_zones)]
    zone_peaks: dict[str, float] = {}
    readings: list[float] = []
    for sample in active:
        for zone in sample.thermal_zones:
            if zone.temp_c is not None:
                readings.append(zone.temp_c)
                if zone.zone in inventory:
                    zone_peaks[zone.zone] = max(zone_peaks.get(zone.zone, zone.temp_c), zone.temp_c)
    thermal = _observed(_timeline(thermal_times, window), len(active))
    thermal_state = (ThermalState(max(readings), tuple(ThermalZone(name, zone_peaks.get(name))
                                                     for name in sorted(inventory))) if readings else None)
    intervals: list[tuple[int, int, int]] = []
    for previous, current in zip(active, active[1:]):
        before, after = previous.receiver_cpu_ticks, current.receiver_cpu_ticks
        if before is not None and after is not None and after >= before:
            intervals.append((previous.t_ns, current.t_ns, after - before))
    duration = window.end_ns - window.start_ns if window else 0
    measured = sum(end - start for start, end, _ in intervals)
    uncovered = duration
    if intervals and window:
        uncovered = max(intervals[0][0] - window.start_ns, window.end_ns - intervals[-1][1],
                        max((b[0] - a[1] for a, b in zip(intervals, intervals[1:])), default=0))
    baseline = bool(active and active[0].receiver_cpu_ticks is not None)
    cpu = CpuCoverage(max(0, len(active) - 1), len(intervals), max(0, len(active) - 1) - len(intervals),
                      intervals[0][0] if intervals else None, intervals[-1][1] if intervals else None,
                      uncovered, baseline, measured)
    video = evidence.video_evidence
    value = Coverage(POLICY, window, sampling, rss, cpu, thermal, *endpoints,
                     QuantileScope(QUANTILE_POLICY, LIMIT, len(video.ipc_latency_ns), len(video.decode_latency_ns)))
    _validate_coverage(value)
    growth = None
    if len(active) >= 2:
        initial, final = active[0].receiver_rss_mb, active[-1].receiver_rss_mb
        if initial is not None and initial > 0 and final is not None:
            growth = (final - initial) / initial * 100
    cpu_percentages = [100 * delta * 1e9 / evidence.cpu_ticks_per_second / (end - start)
                       for start, end, delta in intervals]
    sustained = None
    if baseline and len(active) >= 2 and cpu.missing == 0 and duration > 0:
        sustained = 100 * sum(delta for _, _, delta in intervals) * 1e9 / evidence.cpu_ticks_per_second / duration
    consumption, _, decode = endpoints
    ipc_span = consumption.last_ns - consumption.begin_ns if consumption.last_ns is not None and consumption.begin_ns is not None else 0
    decode_span = decode.last_ns - decode.begin_ns if decode.last_ns is not None and decode.begin_ns is not None else 0
    metrics: dict[str, int | float | None] = {
        "receiver_rss_peak_mb": max(rss_values) if rss_values else None,
        "receiver_rss_mean_mb": sum(rss_values) / len(rss_values) if rss_values else None,
        "rss_growth_pct": growth, "receiver_cpu_sustained_pct_one_core": sustained,
        "receiver_cpu_peak_pct_one_core": max(cpu_percentages) if cpu_percentages else None,
        "thermal_peak_c": thermal_state.peak_c if thermal_state else None,
        "decode_frames": decode.count if decode.count else None,
        "decode_fps": decode.count * 1e9 / decode_span if decode.count and decode_span else None,
        "ipc_throughput_mbps": consumption.bytes * 8000 / ipc_span if consumption.count and ipc_span else None,
        "decode_p50_ms": _quantile(video.decode_latency_ns, 0.5),
        "decode_p95_ms": _quantile(video.decode_latency_ns, 0.95),
        "ipc_p95_added_latency_ms": _quantile(video.ipc_latency_ns, 0.95),
    }
    return Reduction(value, MappingProxyType(metrics), thermal_state)


def _quantile(ring: tuple[int, ...], fraction: float) -> float | None:
    return sorted(ring)[math.ceil(fraction * len(ring)) - 1] / 1e6 if ring else None
