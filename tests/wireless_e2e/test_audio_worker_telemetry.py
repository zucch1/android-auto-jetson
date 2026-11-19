# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Validate wireless audio runtime JSONL output against collector and smoke schemas."""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    binary = sys.argv[2]

    sys.path.insert(0, str(root / "tools" / "perf"))
    sys.path.insert(0, str(root / "tools" / "wired-qa"))
    import aa_drive_smoke as smoke  # noqa: PLC0415
    import aa_projection_collect as collector  # noqa: PLC0415

    with tempfile.TemporaryDirectory(prefix="aa-wr-telemetry-") as directory:
        work = Path(directory)
        subprocess.run(
            [sys.executable, "-B", str(root / "tools" / "tls" / "synthetic_credential.py"),
             str(work / "generated")],
            capture_output=True, text=True, check=True,
        )
        generated = work / "generated"
        bundle = work / "sensitive-credential.pem"
        bundle.write_bytes(
            (generated / "synthetic-hu.crt").read_bytes()
            + (generated / "synthetic-hu.key").read_bytes()
        )
        bundle.chmod(0o600)

        out_base = work / "events.jsonl"
        env = dict(os.environ)
        env["HU_KEY_PATH"] = str(bundle)
        env["AA_TEST_JSONL_OUT"] = str(out_base)

        filt = (
            "WirelessAudioRuntime.QuietTcpLinkDefersAudioCompletionAtFiveMsCadence:"
            "WirelessAudioRuntime.ReconnectWithInFlightSessionOneSinkWriteIsolatesEpochTwo:"
            "WirelessAudioRuntime.RuntimeAvPendingDuplicateBeforeAckEmitsQueueRejectionAndCreditTrace:"
            "WirelessAudioRuntime.UnregisteredChannelPollErrorEmitsRejectedFrameFields"
        )
        proc = subprocess.run(
            [binary, f"--gtest_filter={filt}"],
            env=env, capture_output=True, text=True, timeout=60, check=False,
        )
        assert proc.returncode == 0, proc.stdout + proc.stderr

        # 1. Quiet 5 ms run: real audio_write_completed + presented_ns parses cleanly
        #    with 0 invalid_telemetry and 0 terminal_errors in EventParser & aa_drive_smoke.
        quiet_path = Path(f"{out_base}.quiet_5ms")
        assert quiet_path.exists()
        quiet_parser = collector.EventParser()
        quiet_parser.poll(str(quiet_path))
        assert quiet_parser.invalid_telemetry == [], quiet_parser.invalid_telemetry
        assert quiet_parser.terminal_errors == [], quiet_parser.terminal_errors
        assert quiet_parser.audio_writes >= 3, quiet_parser.audio_writes
        assert quiet_parser.metrics["audio_latency_ms"] is not None
        assert quiet_parser.metrics["audio_scheduling_skew_ms"] is not None

        quiet_events = smoke.read_jsonl(quiet_path)
        audio_events = [e for e in quiet_events if e.get("event") == "audio_write_completed"]
        valid_audio, invalid_audio = smoke._valid_audio(audio_events)
        assert len(valid_audio) >= 3 and invalid_audio == 0, (len(valid_audio), invalid_audio)
        assert any(e.get("event") == "media_timing" for e in quiet_events)

        # 2. Reconnect across epoch 1 -> 2: 0 terminal errors, clean audio_chunks progress.
        reconnect_path = Path(f"{out_base}.reconnect_epoch")
        assert reconnect_path.exists()
        reconnect_parser = collector.EventParser()
        reconnect_parser.poll(str(reconnect_path))
        assert reconnect_parser.invalid_telemetry == [], reconnect_parser.invalid_telemetry
        assert reconnect_parser.terminal_errors == [], reconnect_parser.terminal_errors
        assert reconnect_parser.audio_chunks >= 1, reconnect_parser.audio_chunks
        assert reconnect_parser.audio_writes >= 1, reconnect_parser.audio_writes

        # 3. av_pending duplicate before ACK: poll error with queue_rejection + av_credit_trace.
        pending_path = Path(f"{out_base}.av_pending")
        assert pending_path.exists()
        pending_parser = collector.EventParser()
        pending_parser.poll(str(pending_path))
        assert pending_parser.terminal_errors == [{"event": "err", "code": "ipc_queue_full"}], (
            pending_parser.terminal_errors
        )
        pending_events = smoke.read_jsonl(pending_path)
        poll_errs = [
            e for e in pending_events
            if e.get("event") == "err" and e.get("detail") == "poll"
            and e.get("code") == "ipc_queue_full"
        ]
        assert len(poll_errs) == 1, poll_errs
        assert poll_errs[0]["queue_rejection"]["source"] == "av_pending", poll_errs[0]
        assert isinstance(poll_errs[0].get("av_credit_trace"), dict), poll_errs[0]

        # 4. Unregistered channel poll error: poll error with reject_channel + reject_msg_id.
        unreg_path = Path(f"{out_base}.unregistered_channel")
        assert unreg_path.exists()
        unreg_parser = collector.EventParser()
        unreg_parser.poll(str(unreg_path))
        assert unreg_parser.terminal_errors == [
            {"event": "err", "code": "channel_not_registered"}
        ], unreg_parser.terminal_errors
        unreg_events = smoke.read_jsonl(unreg_path)
        unreg_errs = [
            e for e in unreg_events
            if e.get("event") == "err" and e.get("detail") == "poll"
            and e.get("code") == "channel_not_registered"
        ]
        assert len(unreg_errs) == 1, unreg_errs
        assert unreg_errs[0].get("reject_channel") == 120, unreg_errs[0]
        assert unreg_errs[0].get("reject_msg_id") == 32773, unreg_errs[0]

    print("wireless-audio-worker-telemetry=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
