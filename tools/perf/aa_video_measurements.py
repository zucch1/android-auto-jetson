# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded side-band video endpoint reduction; never uses AV time as a duration."""
from __future__ import annotations

from collections import OrderedDict, deque
from collections.abc import Mapping
from dataclasses import dataclass
from math import ceil
from typing import Final, Literal, TypeAlias, assert_never

from aa_measurement_coverage import Contribution, CoverageLedger, VideoEvidence

Key: TypeAlias = tuple[int, ...]
MetricValues: TypeAlias = dict[str, int | float]
EventFields: TypeAlias = Mapping[str, int | float | str | bool | None]
LIMIT: Final = 4096
RETENTION_NS: Final = 10_000_000_000


def _unsigned_fields(raw: EventFields, keys: tuple[str, ...]) -> tuple[int, ...] | None:
    values: list[int] = []
    for key in keys:
        value = raw.get(key)
        if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value < 2**64:
            return None
        values.append(value)
    return tuple(values)


@dataclass(frozen=True, slots=True)
class IpcEndpoint:
    key: Key
    connection: int
    observed: int
    size: int

    @staticmethod
    def parse(raw: EventFields) -> IpcEndpoint | None:
        keys = ("producer_pid", "consumer_pid", "frame", "sequence", "timestamp_ns",
                "connection_ns", "observed_ns", "payload_bytes")
        values = _unsigned_fields(raw, keys)
        if values is None:
            return None
        producer, consumer, frame, sequence, stamp, connection, observed, size = values
        if producer == 0 or consumer == 0 or size == 0 or connection > observed:
            return None
        return IpcEndpoint((producer, consumer, frame, sequence, stamp), connection, observed, size)


@dataclass(frozen=True, slots=True)
class DecodeEndpoint:
    key: Key
    submitted: int
    completed: int

    @staticmethod
    def parse(raw: EventFields) -> DecodeEndpoint | None:
        keys = ("session_ns", "frame", "sequence", "timestamp_ns", "submitted_ns", "completed_ns")
        values = _unsigned_fields(raw, keys)
        if values is None:
            return None
        session, frame, sequence, stamp, submitted, completed = values
        if not session <= submitted <= completed or completed - submitted >= 5_000_000_000:
            return None
        return DecodeEndpoint((session, frame, sequence, stamp), submitted, completed)


class VideoMeasurements:
    """Mutable endpoint admission state; sealed summaries outlive bounded join tables."""

    def __init__(self) -> None:
        self.sends: OrderedDict[Key, IpcEndpoint] = OrderedDict()
        self.receives: OrderedDict[Key, IpcEndpoint] = OrderedDict()
        self.joined: OrderedDict[Key, int] = OrderedDict()
        self.decoded: OrderedDict[Key, int] = OrderedDict()
        self.ipc_samples: deque[int] = deque(maxlen=LIMIT)
        self.ipc_matched = 0
        self.decode_samples: deque[int] = deque(maxlen=LIMIT)
        self.watermark = 0
        self.decode_watermark = 0
        self.decode_frames = 0
        self.decode_begin: int | None = None
        self.decode_end = 0
        self.receive_begin: int | None = None
        self.receive_end = 0
        self.receive_bytes = 0
        self.receive_seen: OrderedDict[Key, int] = OrderedDict()
        self.bindings: OrderedDict[Key, int] = OrderedDict()
        self.unmatched = 0
        self.origin_ns = 0
        self.start_ns = 0
        self.sealed_end = -1
        self.boundaries: deque[int] = deque()
        self.ledger: CoverageLedger | None = None
        self.frozen = False
        self.capacity_lost = False
        self.startup_decode: deque[DecodeEndpoint] = deque(maxlen=LIMIT)

    @property
    def issues(self) -> tuple[str, ...]:
        issues = self.ledger.issues if self.ledger is not None else ()
        return tuple(sorted(set(issues + (("coverage_capacity",) if self.capacity_lost else ()))))

    def start_window(self, origin_ns: int, start_ns: int) -> None:
        sends, receives, decoded = tuple(self.sends.values()), tuple(self.receives.values()), tuple(self.startup_decode)
        self.__init__()
        self.origin_ns, self.start_ns = origin_ns, start_ns
        self.sealed_end = start_ns - 1
        self.ledger = CoverageLedger(start_ns)
        for endpoint in sends:
            self.ipc("send", endpoint)
        for endpoint in receives:
            self.ipc("receive", endpoint)
        for endpoint in decoded:
            self.decode(endpoint)

    def advance(self, sample_ns: int) -> None:
        if self.ledger is None or self.frozen:
            return
        self.ledger.advance(sample_ns)
        self.boundaries.append(sample_ns)
        while self.boundaries and self.boundaries[0] <= sample_ns - RETENTION_NS:
            self.sealed_end = self.boundaries.popleft()
        floor = self.origin_ns + self.sealed_end
        for table in (self.joined, self.decoded, self.receive_seen):
            for key, observed in list(table.items()):
                if observed <= floor:
                    del table[key]
        for key, endpoint in list(self.receives.items()):
            if endpoint.observed <= floor:
                del self.receives[key]
                self.unmatched += 1

    def finish(self, end_ns: int) -> VideoEvidence:
        self.frozen = True
        blocks = self.ledger.finish(end_ns) if self.ledger is not None else ()
        return VideoEvidence(blocks, tuple(self.ipc_samples), tuple(self.decode_samples))

    def _room(self, table: Mapping[Key, IpcEndpoint | int]) -> bool:
        if len(table) < LIMIT:
            return True
        self.capacity_lost = True
        return False

    @staticmethod
    def _percentile(samples: deque[int], fraction: float) -> float:
        ordered = sorted(samples)
        return ordered[ceil(len(ordered) * fraction) - 1] / 1e6

    def ipc(self, direction: Literal["send", "receive"], endpoint: IpcEndpoint) -> MetricValues:
        if self.frozen:
            return {}
        match direction:
            case "send":
                own, peer = self.sends, self.receives
                sending = True
            case "receive":
                own, peer = self.receives, self.sends
                sending = False
            case unreachable:
                assert_never(unreachable)
        result: MetricValues = {}
        if self.ledger is None:
            own[endpoint.key] = endpoint
            if len(own) > LIMIT:
                own.popitem(last=False)
            return result
        stamp = endpoint.observed - self.origin_ns
        if stamp < self.start_ns or (not sending and stamp <= self.sealed_end):
            return result
        if not sending and endpoint.key not in self.receive_seen and self._room(self.receive_seen):
            begin = max(self.start_ns, endpoint.connection - self.origin_ns)
            if self.ledger.add(Contribution("ipc_consumption", begin, stamp, endpoint.size)):
                self.receive_seen[endpoint.key] = endpoint.observed
                self.receive_bytes += endpoint.size
                self.receive_begin = begin if self.receive_begin is None else min(self.receive_begin, begin)
                self.receive_end = max(self.receive_end, stamp)
        if self.receive_begin is not None and self.receive_end > self.receive_begin:
            result["ipc_throughput_mbps"] = self.receive_bytes * 8000 / (
                self.receive_end - self.receive_begin)
        self.watermark = max(self.watermark, endpoint.observed)
        cutoff = self.watermark - RETENTION_NS
        for pending in (self.sends, self.receives):
            for key, value in list(pending.items()):
                if value.observed <= cutoff:
                    del pending[key]
                    self.unmatched += 1
        if endpoint.observed <= cutoff:
            self.unmatched += 1
            return result
        if endpoint.key in self.joined or endpoint.key in own:
            return result
        counterpart = peer.pop(endpoint.key, None)
        if counterpart is None:
            if self._room(own):
                own[endpoint.key] = endpoint
            else:
                self.unmatched += 1
            return result
        send, receive = ((endpoint, counterpart) if sending else
                         (counterpart, endpoint))
        if receive.key not in self.receive_seen:
            return result
        if not self._room(self.joined):
            return result
        self.joined[endpoint.key] = receive.observed
        producer = (0, *send.key[:2], send.connection)
        consumer = (1, *receive.key[:2], receive.connection)
        consistent = (self.bindings.get(producer, receive.connection) == receive.connection
                      and self.bindings.get(consumer, send.connection) == send.connection)
        delta = receive.observed - send.observed
        if (send.size != receive.size or not 0 <= delta < RETENTION_NS or not consistent
                or not receive.connection <= send.connection <= send.observed):
            self.unmatched += 1
            return result
        new_bindings = sum(key not in self.bindings for key in (producer, consumer))
        if len(self.bindings) + new_bindings > LIMIT:
            self.capacity_lost = True
            return result
        if not self.ledger.add(Contribution("ipc_matches", send.observed - self.origin_ns,
                                            receive.observed - self.origin_ns, 0)):
            return result
        self.bindings[producer] = receive.connection
        self.bindings[consumer] = send.connection
        self.ipc_samples.append(delta)
        self.ipc_matched += 1
        result["ipc_p95_added_latency_ms"] = self._percentile(self.ipc_samples, .95)
        return result

    def decode(self, endpoint: DecodeEndpoint) -> MetricValues:
        if self.frozen:
            return {}
        if self.ledger is None:
            self.startup_decode.append(endpoint)
            return {}
        self.decode_watermark = max(self.decode_watermark, endpoint.completed)
        cutoff = max(self.decode_watermark - RETENTION_NS, self.origin_ns + self.sealed_end)
        if endpoint.completed < cutoff or endpoint.key in self.decoded:
            return {}
        if endpoint.submitted - self.origin_ns < self.start_ns or not self._room(self.decoded):
            return {}
        if not self.ledger.add(Contribution("decode", endpoint.submitted - self.origin_ns,
                                            endpoint.completed - self.origin_ns, 0)):
            return {}
        self.decoded[endpoint.key] = endpoint.completed
        self.decode_frames += 1
        self.decode_begin = (endpoint.submitted if self.decode_begin is None else
                             min(self.decode_begin, endpoint.submitted))
        self.decode_end = max(self.decode_end, endpoint.completed)
        self.decode_samples.append(endpoint.completed - endpoint.submitted)
        result: MetricValues = {
            "decode_frames": self.decode_frames,
            "decode_p50_ms": self._percentile(self.decode_samples, .5),
            "decode_p95_ms": self._percentile(self.decode_samples, .95),
        }
        if self.decode_end > self.decode_begin:
            result["decode_fps"] = self.decode_frames * 1e9 / (self.decode_end - self.decode_begin)
        return result
