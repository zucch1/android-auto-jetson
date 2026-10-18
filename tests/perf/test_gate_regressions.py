#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Oracle-gate regressions, also invoked by the existing eight CTest groups."""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
import aa_perf_report
import aa_perf_schema
import aa_projection_collect
from aa_measurement_coverage import CoverageError
from test_v2_coverage import synthetic


def result(capture: dict) -> dict:
    return aa_perf_report.build_result(json.loads(json.dumps(capture)), duration_requested="30m",
                                      now="2026-10-14T00:00:00Z", capture_path=None,
                                      capture_sha256=None)


def nominal() -> dict:
    return json.loads((ROOT / "tests/perf/fixtures/wired_perf/nominal_capture.json").read_text())


def finite(tmp: Path) -> None:
    for token in ("NaN", "Infinity", "-Infinity", "1e999"):
        parser = aa_projection_collect.EventParser()
        parser._consume('{"event":"media_timing","av_sync_ms":12,"audio_latency_ms":40}')
        parser._consume('{"event":"media_timing","av_sync_ms":' + token + '}')
        assert parser.metrics["av_sync_ms"] is None, token
        capture = nominal()
        capture["events"]["av_sync_ms"] = json.loads(token)
        doc = result(capture)
        assert doc["verdict"] == "BLOCKED", token
        json.dumps(doc, allow_nan=False)
        assert aa_perf_schema.validate(json.loads(token), {"type": "number"}), token
        # Permissive object schemas must not admit nested non-finite numbers either.
        assert aa_perf_schema.validate({"nested": [json.loads(token)]}, {"type": "object"})
        env = dict(os.environ, FAKE_RECEIVER_RAW_TELEMETRY=
                   '{"event":"media_timing","av_sync_ms":' + token + '}')
        proc = subprocess.run([sys.executable, "-B", str(ROOT / "tools/perf/aa_projection_collect.py"),
                               "--receiver", str(ROOT / "tests/perf/fixtures/wired_perf/fake_receiver.py"),
                                "--port", "fixture", "--no-display", "--duration-seconds", "1",
                                "--startup-timeout-seconds", "1",
                               "--interval", "0.1", "--state-dir", str(tmp / ("raw-" + token))],
                              env=env, capture_output=True, text=True, timeout=15)
        assert proc.returncode == 0, proc.stderr
        collected = json.loads(proc.stdout)
        assert collected["events"]["av_sync_ms"] is None, token
        assert "av_sync_ms" in collected["invalid_telemetry"], token
        assert result(collected)["verdict"] == "BLOCKED", token
        json.dumps(collected, allow_nan=False)
    capture = nominal()
    capture["samples"][0]["receiver_rss_mb"] = float("nan")
    try:
        result(capture)
    except CoverageError:
        pass
    else:
        raise AssertionError("nonfinite raw measurement accepted")
    parser = aa_projection_collect.EventParser()
    parser._consume('{"event":"decode_stats","frames":1e999}')
    assert parser.metrics["decode_frames"] is None


def interruption(tmp: Path) -> None:
    for field, value in (("early_exit", True), ("receiver_exit_code", 2),
                         ("display_survived", False),
                         ("terminal_errors", [{"event": "err", "code": "transport_io"}])):
        capture = nominal()
        capture[field] = value
        capture["observed_duration_seconds"] = 2.5
        doc = result(capture)
        assert doc["verdict"] != "PASS", field
        assert doc["duration_seconds"] == 20
        assert doc["run_status"][field] == value
        assert doc["violations"], field
    for scenario in ("clean_early", "terminal_alive", "display_death"):
        receiver = tmp / (scenario + "-receiver")
        receiver.write_text('''#!/usr/bin/env python3
import json, time
print('{"event":"ready","state":"negotiating"}', flush=True)
print('{"event":"active","startup_ms":5}', flush=True)
print('{"event":"video_sent","units":10}', flush=True)
print('{"event":"audio_chunks","count":10}', flush=True)
print('{"event":"audio_write_completed","scheduled_ns":1000000000,"scheduled_end_ns":1020000000,"completed_ns":1000000000}', flush=True)
print('{"event":"video_presented","scheduled_ns":1000000000,"completed_ns":1012000000}', flush=True)
print('{"event":"media_timing","av_sync_ms":12,"audio_latency_ms":40}', flush=True)
time.sleep(0.35)
''' + ("raise SystemExit(0)\n" if scenario == "clean_early" else
       "print('{\"event\":\"err\",\"code\":\"transport_io\"}', flush=True)\nwhile True: time.sleep(0.01)\n"
       if scenario == "terminal_alive" else "while True: time.sleep(0.01)\n"))
        receiver.chmod(0o755)
        display = tmp / "display"
        display.write_text("#!/bin/bash\nset -euo pipefail\nsleep 0.25\nexit 0\n")
        display.chmod(0o755)
        args = argparse.Namespace(receiver=str(receiver), display=str(display),
                                  no_display=scenario != "display_death", port="fixture",
                                  duration_seconds=1, interval=0.1, store=None,
                                  video_socket=None, input_socket=None,
                                  state_dir=str(tmp / scenario))
        with patch.object(aa_projection_collect, "_display_connected", return_value=True):
            capture = aa_projection_collect.collect(args)
        doc = result(capture)
        failure = {"clean_early": "early_exit", "terminal_alive": "terminal_errors",
                   "display_death": "display_survived"}[scenario]
        assert failure in {v["metric"] for v in doc["violations"]}, scenario
        assert doc["duration_seconds"] < 1, scenario
        assert doc["run_status"]["startup_ms"] == 5, scenario
        assert doc["metrics"]["av_sync_ms"]["value"] == 12, scenario


def completeness(tmp: Path) -> None:
    gaps = ["frame_drops", "decode_p50_ms", "decode_p95_ms", "decode_fps",
            "decode_frames", "ipc_throughput_mbps", "ipc_p95_added_latency_ms"]
    capture = synthetic(20)
    capture["events"].pop("frame_drops")
    capture["video_evidence"]["blocks"] = [{**block,
        **{stream: {"count": 0, "first_ns": None, "last_ns": None,
                    "max_internal_gap_ns": 0, "begin_ns": None, "bytes": 0}
           for stream in ("ipc_consumption", "ipc_matches", "decode")}}
        for block in capture["video_evidence"]["blocks"]]
    capture["video_evidence"].update(ipc_latency_ns=[], decode_latency_ns=[])
    doc = result(capture)
    assert doc["verdict"] == "BLOCKED"
    assert set(gaps) <= set(doc["named_gaps"])
    assert doc["qualification_status"] == "INCOMPLETE"
    capture = synthetic(1800)
    capture["measurement_origin"] = "live"
    capture["provenance"] = {}
    doc = result(capture)
    assert doc["verdict"] == "BLOCKED"
    assert len(doc["provenance_gaps"]) == 5
    assert result(nominal())["qualification_status"] == "OFFLINE_ONLY"
    capture = synthetic(1800)
    capture.update(measurement_origin="live", observed_duration_seconds=1800,
                   early_exit=False, receiver_exit_code=0, display_survived=True,
                   display_connected=True)
    capture["events"].update(startup_ms=10, video_units=100, audio_chunks=100)
    capture["provenance"].update(source_digest="a" * 64, build_artifact_digest="b" * 64,
                                 configuration_digest="c" * 64,
                                 service_definitions=[{"name": "fixture.service", "digest": "d" * 64}])
    assert result(capture)["qualification_status"] == "COMPLETE_LIVE"
    for name in ("source_digest", "build_artifact_digest", "configuration_digest",
                 "service_definitions", "dependency_versions"):
        broken = json.loads(json.dumps(capture))
        broken["provenance"].pop(name)
        doc = result(broken)
        assert doc["verdict"] == "BLOCKED" and name in doc["provenance_gaps"], name
    out = tmp / "live-provenance.json"
    proc = subprocess.run([sys.executable, "-B", str(ROOT / "tools/perf/aa_perf_report.py"),
                           "--live", "--capture", str(ROOT / "tests/perf/fixtures/wired_perf/nominal_capture.json"),
                           "--out", str(out)], capture_output=True, text=True, timeout=10)
    assert proc.returncode == 3, proc.stderr
    assert json.loads(out.read_text())["measurement_origin"] == "live"


def readiness(tmp: Path) -> None:
    # Execute the actual remote shell expressions in a sandbox, not a prose proxy.
    source = (ROOT / "tools/perf/aa-projection-perf.sh").read_text()
    audio_command = re.search(r'audio_sink="\$\(remote "(.*)" 2>/dev/null', source).group(1)
    audio_command = 'command="' + audio_command + '"; eval "$command"'
    sandbox = tmp / "commands"
    sandbox.mkdir()
    for command in ("head", "grep"):
        (sandbox / command).symlink_to(Path("/usr/bin") / command)
    pw = sandbox / "pw-cli"
    pw.write_text('#!/bin/bash\nset -euo pipefail\nprintf \'%s\\n\' "$PW_RECORD"\n')
    pw.chmod(0o755)
    pactl = sandbox / "pactl"
    for mode in ("absent", "empty", "failed", "sink"):
        if mode != "absent":
            pactl.write_text('#!/bin/bash\nset -euo pipefail\n' +
                             {"empty": "exit 0\n", "failed": "exit 1\n",
                              "sink": "printf '1 output sink\\n'\n"}[mode])
            pactl.chmod(0o755)
        for record, ready in (("id 1 Device", False), ('media.class = "Audio/Sink"', True)):
            env = dict(os.environ, PATH=str(sandbox), PW_RECORD=record)
            proc = subprocess.run(["/bin/bash", "-c", audio_command], env=env,
                                  capture_output=True, text=True, timeout=5)
            assert bool(proc.stdout.strip()) == (ready or mode == "sink"), (mode, record, proc)
    # Isolate check_readiness with remote() supplying connector records.
    function = source[source.index("ready_reason=\"\""):source.index("write_readiness()")]
    for status, ready in (("connected", True), ("disconnected", False), ("unknown", False),
                          ("", False), ("disconnected\nconnected", True),
                          ("disconnected\nunknown", False)):
        script = '''set -euo pipefail
RECEIVER=fixture
remote() {
 case "$1" in
  true|"test -x 'fixture'") return 0 ;;
  "for c"*) printf '%s\\n' "$DRM_RECORD" ;;
  *) printf 'sink\\n' ;;
 esac
}
''' + function + '\ncheck_readiness'
        proc = subprocess.run(["/bin/bash", "-c", script], capture_output=True,
                              env=dict(os.environ, DRM_RECORD=status), timeout=5)
        assert (proc.returncode == 0) == ready, status


def shutdown(tmp: Path) -> None:
    receiver = tmp / "receiver"
    receiver.write_text('''#!/usr/bin/env python3
import json, signal, time
def stop(*args):
 print('{"event":"audio_write_completed","scheduled_ns":2000000000,"scheduled_end_ns":2020000000,"completed_ns":2000000000}', flush=True)
 print('{"event":"video_presented","scheduled_ns":2000000000,"completed_ns":2075000000}', flush=True)
 print('{"event":"media_timing","av_sync_ms":75,"audio_latency_ms":90}', flush=True)
 raise SystemExit(0)
signal.signal(signal.SIGTERM, stop)
print('{"event":"media_timing","av_sync_ms":12,"audio_latency_ms":40}', flush=True)
while True: time.sleep(0.01)
''')
    receiver.chmod(0o755)
    proc = subprocess.run([sys.executable, "-B", str(ROOT / "tools/perf/aa_projection_collect.py"),
                           "--receiver", str(receiver), "--port", "fixture", "--no-display",
                           "--duration-seconds", "1", "--interval", "0.1",
                           "--startup-timeout-seconds", "1",
                           "--state-dir", str(tmp / "state")], capture_output=True, text=True, timeout=15)
    assert proc.returncode == 0, proc.stderr
    capture = json.loads(proc.stdout)
    assert capture["events"]["av_sync_ms"] is None, capture
    assert result(capture)["verdict"] == "BLOCKED"
    assert not result(capture)["violations"]


def receiver_hook_present(source: str) -> bool:
    code = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',
                  lambda m: "" if m.group().startswith("/") else m.group(),
                  source, flags=re.S)
    pump = re.search(r"router_\s*\.\s*pump\s*\(\s*\)", code)
    call = re.search(r"\bemit_media_timing\s*\(\s*\)\s*;", code)
    stats = re.search(r"stats\s*=\s*router_\s*\.\s*report\s*\(\s*\)\s*\.\s*stats", code)
    return bool(pump and call and stats
                and re.search(r"format_ms\s*\(\s*stats\.max_completion_skew\s*\)", code)
                and re.search(r"format_ms\s*\(\s*stats\.max_path_latency\s*\)", code))


CASES = {"finite": finite, "interruption": interruption, "completeness": completeness,
         "readiness": readiness, "shutdown": shutdown}

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="perf-gate-") as base:
        CASES[sys.argv[1]](Path(base))
    print(f"PASS gate-{sys.argv[1]}")
