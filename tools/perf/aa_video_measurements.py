# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded side-band video endpoint reduction; never uses AV time as a duration."""
from __future__ import annotations

from collections import OrderedDict, deque
from collections.abc import Mapping
from dataclasses import dataclass
from math import ceil
from typing import Final, Literal, TypeAlias, assert_never

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
    """Mutable bounded joins and rolling quantiles with cumulative observed counters."""

    def __init__(self) -> None:
        self.sends: OrderedDict[Key, IpcEndpoint] = OrderedDict()
        self.receives: OrderedDict[Key, IpcEndpoint] = OrderedDict()
        self.joined: OrderedDict[Key, int] = OrderedDict()
        self.decoded: OrderedDict[Key, int] = OrderedDict()
        self.ipc_samples: deque[int] = deque(maxlen=LIMIT)
        self.ipc_matched = 0
        self.decode_samples: deque[int] = deque(maxlen=LIMIT)
        self.watermark = 0
        self.correlation_floor = -1
        self.decode_watermark = 0
        self.decode_frames = 0
        self.decode_begin: int | None = None
        self.decode_end = 0
        self.receive_begin: int | None = None
        self.receive_end = 0
        self.receive_floor = -1
        self.receive_bytes = 0
        self.receive_seen: OrderedDict[Key, int] = OrderedDict()
        self.bindings: OrderedDict[Key, int] = OrderedDict()
        self.unmatched = 0

    @staticmethod
    def _percentile(samples: deque[int], fraction: float) -> float:
        ordered = sorted(samples)
        return ordered[ceil(len(ordered) * fraction) - 1] / 1e6

    def ipc(self, direction: Literal["send", "receive"], endpoint: IpcEndpoint) -> MetricValues:
        match direction:
            case "send":
                own, peer = self.sends, self.receives
                sending = True
            case "receive":
                own, peer = self.receives, self.sends
                sending = False
                receive_cutoff = max(self.receive_end - RETENTION_NS, self.receive_floor)
                for key, observed in list(self.receive_seen.items()):
                    if observed <= receive_cutoff:
                        del self.receive_seen[key]
                if endpoint.observed > receive_cutoff and endpoint.key not in self.receive_seen:
                    self.receive_seen[endpoint.key] = endpoint.observed
                    self.receive_bytes += endpoint.size
                    self.receive_begin = (endpoint.connection if self.receive_begin is None else
                                          min(self.receive_begin, endpoint.connection))
                    self.receive_end = max(self.receive_end, endpoint.observed)
                    if len(self.receive_seen) > LIMIT:
                        _, evicted_at = self.receive_seen.popitem(last=False)
                        self.receive_floor = max(self.receive_floor, evicted_at)
            case unreachable:
                assert_never(unreachable)
        result: MetricValues = {}
        if self.receive_begin is not None and self.receive_end > self.receive_begin:
            result["ipc_throughput_mbps"] = self.receive_bytes * 8000 / (
                self.receive_end - self.receive_begin)
        self.watermark = max(self.watermark, endpoint.observed)
        cutoff = max(self.watermark - RETENTION_NS, self.correlation_floor)
        for pending in (self.sends, self.receives):
            for key, value in list(pending.items()):
                if value.observed <= cutoff:
                    del pending[key]
                    self.unmatched += 1
        for key, observed in list(self.joined.items()):
            if observed <= cutoff:
                del self.joined[key]
        if endpoint.observed <= cutoff:
            self.unmatched += 1
            return result
        if endpoint.key in self.joined or endpoint.key in own:
            return result
        counterpart = peer.pop(endpoint.key, None)
        if counterpart is None:
            own[endpoint.key] = endpoint
            if len(own) > LIMIT:
                _, evicted = own.popitem(last=False)
                self.correlation_floor = max(self.correlation_floor, evicted.observed)
                self.unmatched += 1
            return result
        send, receive = ((endpoint, counterpart) if sending else
                         (counterpart, endpoint))
        self.joined[endpoint.key] = receive.observed
        if len(self.joined) > LIMIT:
            self.joined.popitem(last=False)
        producer = (0, *send.key[:2], send.connection)
        consumer = (1, *receive.key[:2], receive.connection)
        consistent = (self.bindings.get(producer, receive.connection) == receive.connection
                      and self.bindings.get(consumer, send.connection) == send.connection)
        delta = receive.observed - send.observed
        if (send.size != receive.size or not 0 <= delta < RETENTION_NS or not consistent
                or not receive.connection <= send.connection <= send.observed):
            self.unmatched += 1
            return result
        self.bindings[producer] = receive.connection
        self.bindings[consumer] = send.connection
        while len(self.bindings) > LIMIT:
            self.bindings.popitem(last=False)
        self.ipc_samples.append(delta)
        self.ipc_matched += 1
        result["ipc_p95_added_latency_ms"] = self._percentile(self.ipc_samples, .95)
        return result

    def decode(self, endpoint: DecodeEndpoint) -> MetricValues:
        self.decode_watermark = max(self.decode_watermark, endpoint.completed)
        cutoff = self.decode_watermark - RETENTION_NS
        for key, observed in list(self.decoded.items()):
            if observed < cutoff:
                del self.decoded[key]
        if endpoint.completed < cutoff or endpoint.key in self.decoded:
            return {}
        self.decoded[endpoint.key] = endpoint.completed
        if len(self.decoded) > LIMIT:
            self.decoded.popitem(last=False)
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
