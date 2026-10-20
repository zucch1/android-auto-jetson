# SPDX-License-Identifier: GPL-3.0-or-later
"""Explicit live-shaped v2 evidence, invented entirely for contract tests."""
from __future__ import annotations

from common import JSON


def capture(seconds: int, provenance: dict[str, JSON]) -> dict[str, JSON]:
    """Create every active sample/block; never promote an offline result."""
    samples: list[JSON] = []
    blocks: list[JSON] = []
    start = 1_000_000_000
    for index in range(seconds // 5 + 1):
        stamp = start + index * 5_000_000_000
        samples.append({
            't_ns': stamp, 'receiver_rss_mb': 120.0,
            'receiver_cpu_ticks': index * 100,
            'thermal_zones': [{'zone': 'thermal_zone0:CPU', 'temp_c': 43.0}],
            'projection_active': True,
            **{key: index + 1 for key in ('video_units', 'audio_chunks',
                'video_presentations', 'audio_writes', 'av_pairs')},
        })
        if index:
            streams: dict[str, JSON] = {}
            for name, latency, size in (('ipc_consumption', 5_000_000_000, 4096),
                                        ('ipc_matches', 1_000_000, 0),
                                        ('decode', 100_000_000, 0)):
                streams[name] = {
                    'count': 1, 'first_ns': stamp, 'last_ns': stamp,
                    'max_internal_gap_ns': 0, 'begin_ns': stamp - latency, 'bytes': size,
                }
            blocks.append({'start_ns': stamp - 5_000_000_000, 'end_ns': stamp, **streams})
    retained = min(4096, seconds // 5)
    return {
        'schema': 'aa-projection-perf-capture/2', 'mode': 'wired',
        'measurement_origin': 'live', 'duration_seconds': seconds,
        'observed_duration_seconds': seconds + 1, 'active_projection_seconds': seconds,
        'monotonic_origin_ns': 100_000_000_000, 'cpu_ticks_per_second': 100,
        'crashes': 0, 'early_exit': False, 'receiver_exit_code': 0,
        'receiver_stopped_by_collector': True, 'display_survived': True,
        'display_connected': True, 'terminal_errors': [], 'invalid_telemetry': [],
        'samples': samples, 'video_evidence': {
            'blocks': blocks, 'ipc_latency_ns': [1_000_000] * retained,
            'decode_latency_ns': [100_000_000] * retained,
        },
        'events': {'startup_ms': 900, 'video_units': seconds // 5 + 1,
                   'audio_chunks': seconds // 5 + 1, 'frame_drops': 0,
                   'av_sync_ms': 12.0, 'audio_latency_ms': 84.0,
                   'audio_scheduling_skew_ms': 1.0, 'helper_rss_mb': None},
        'provenance': provenance,
    }
