#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Task-32 wired performance tooling tests.

Runs one case (argv[1]) and exits 0 if all its assertions pass, 1 otherwise:

  schema       valid result passes the schema; malformed documents and
               unsupported schema keywords fail closed
  nominal      the nominal fixture reports PASS (exit 0) and the CLI writes a
               schema-valid result
  budget_fail  the overload fixture reports FAIL (exit 1) with named
               violations; boundary values are enforced exactly
  duration     strict duration/mode parsing: bad input is a usage error with
               no abort and no output file; valid units parse
  contract     plan-mandated contracts and adversarial inputs: helper RSS must
               be null, package_digest null, equivalence_status provisional;
               malformed feeds and misleading-success paths fail closed
  collector    the on-target collector against a deterministic fake receiver:
               fresh state, crash detection, RSS growth, and the fail-closed
               BLOCKED verdict when budgeted metrics are unavailable

Real measurement runs are phase B (see docs/qualification/wired-perf-runbook.md);
these tests use fixtures and a fake receiver only.
"""
from __future__ import annotations

import copy
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "perf"
FIXTURES = ROOT / "tests" / "perf" / "fixtures" / "wired_perf"
SCHEMA = TOOL / "aa-projection-perf.schema.json"
SHELL = TOOL / "aa-projection-perf.sh"
REPORTER = TOOL / "aa_perf_report.py"
VALIDATOR = TOOL / "aa_perf_schema.py"
COLLECTOR = TOOL / "aa_projection_collect.py"
FAKE_RECEIVER = FIXTURES / "fake_receiver.py"
NOMINAL = FIXTURES / "nominal_capture.json"
OVERLOAD = FIXTURES / "overload_capture.json"
NOW = "2026-10-13T00:00:00Z"

sys.path.insert(0, str(TOOL))
import aa_perf_schema  # noqa: E402
import aa_perf_report  # noqa: E402


def run(argv: list[str], env: dict | None = None, timeout: int = 120) -> subprocess.CompletedProcess:
    return subprocess.run(argv, capture_output=True, text=True, timeout=timeout, env=env)


def report(capture: Path, out: Path, extra: list[str] | None = None) -> subprocess.CompletedProcess:
    argv = [sys.executable, "-B", str(REPORTER), "--capture", str(capture), "--out", str(out),
            "--now", NOW]
    if extra:
        argv += extra
    return run(argv)


def shell(args: list[str], timeout: int = 60) -> subprocess.CompletedProcess:
    return run(["bash", str(SHELL), *args], timeout=timeout)


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write(path: Path, doc: object) -> None:
    path.write_text(json.dumps(doc), encoding="utf-8")


def make_capture(tmp: Path, name: str, *, rss0: float, rss1: float, cpu: float, av: float,
                 audio: float, crashes: int = 0) -> Path:
    path = tmp / name
    write(path, {
        "schema": "aa-projection-perf-capture/1",
        "mode": "wired",
        "duration_seconds": 1800,
        "crashes": crashes,
        "samples": [
            {"t_ms": 0, "receiver_rss_mb": rss0, "receiver_cpu_pct_one_core": cpu,
             "thermal_peak_c": 45.0},
            {"t_ms": 1800000, "receiver_rss_mb": rss1, "receiver_cpu_pct_one_core": cpu,
             "thermal_peak_c": 46.0},
        ],
        "events": {"av_sync_ms": av, "audio_latency_ms": audio, "helper_rss_mb": None},
    })
    return path


def case_schema(tmp: Path) -> None:
    result_path = tmp / "result.json"
    assert report(NOMINAL, result_path).returncode == 0, "nominal fixture must report"
    assert run([sys.executable, "-B", str(VALIDATOR), "validate", str(SCHEMA),
                str(result_path)]).returncode == 0, "valid result must validate"

    schema = aa_perf_schema.load_schema(SCHEMA)
    base = load(result_path)
    mutations = {
        "missing_required_top": lambda d: d.pop("budgets"),
        "wrong_duration_type": lambda d: d.__setitem__("duration_seconds", "1800"),
        "extra_top_property": lambda d: d.__setitem__("surprise", 1),
        "bad_verdict_enum": lambda d: d.__setitem__("verdict", "GREEN"),
        "metric_missing_status": lambda d: d["metrics"]["receiver_rss_peak_mb"].pop("status"),
        "metric_bad_value_type": lambda d: d["metrics"]["receiver_rss_peak_mb"].__setitem__(
            "value", "big"),
        "package_digest_non_null": lambda d: d["provenance"].__setitem__("package_digest", "abc"),
        "equivalence_not_provisional": lambda d: d["provenance"].__setitem__(
            "equivalence_status", "established"),
        "thermal_zone_missing_temp": lambda d: d["thermal_state"]["zones"][0].pop("temp_c"),
    }
    for name, mutate in mutations.items():
        doc = copy.deepcopy(base)
        mutate(doc)
        errors = aa_perf_schema.validate(doc, schema)
        assert errors, f"malformed document accepted: {name}"
        bad = tmp / f"bad-{name}.json"
        write(bad, doc)
        assert run([sys.executable, "-B", str(VALIDATOR), "validate", str(SCHEMA),
                    str(bad)]).returncode == 1, f"CLI accepted malformed document: {name}"

    try:
        aa_perf_schema.validate({}, {"type": "object", "typoKeyword": 1})
        raise AssertionError("unknown schema keyword was not rejected")
    except aa_perf_schema.SchemaError:
        pass
    try:
        aa_perf_schema.validate({}, {"$ref": "#/$defs/nope"})
        raise AssertionError("unresolvable $ref was not rejected")
    except aa_perf_schema.SchemaError:
        pass

    schema_budgets = schema["$defs"]["budgets"]["properties"]
    for name, value in aa_perf_report.BUDGETS.items():
        assert name in schema_budgets, f"budget {name} missing from schema"
        assert schema_budgets[name].get("const") == value, (
            f"schema does not pin {name} to the contractual value {value}; "
            "budgets must never be weakened"
        )
        weakened = copy.deepcopy(base)
        weakened["budgets"][name] = value + 1
        assert aa_perf_schema.validate(weakened, schema), (
            f"schema accepted a weakened {name}"
        )

    # Untrusted device text (a thermal zone name) must be JSON-escaped data, not
    # injected into the document structure.
    hostile = tmp / "hostile-capture.json"
    write(hostile, {
        "schema": "aa-projection-perf-capture/1", "mode": "wired", "duration_seconds": 60,
        "crashes": 0,
        "samples": [
            {"t_ms": 0, "receiver_rss_mb": 100.0, "receiver_cpu_pct_one_core": 10.0,
             "thermal_zones": [{"zone": 'cpu"]},"pwned":true', "temp_c": 44.0}]},
            {"t_ms": 60000, "receiver_rss_mb": 100.0, "receiver_cpu_pct_one_core": 10.0,
             "thermal_zones": [{"zone": 'cpu"]},"pwned":true', "temp_c": 44.0}]},
        ],
        "events": {"av_sync_ms": 5.0, "audio_latency_ms": 10.0, "helper_rss_mb": None},
    })
    hostile_out = tmp / "hostile-result.json"
    assert report(hostile, hostile_out).returncode == 0
    hostile_doc = load(hostile_out)
    assert "pwned" not in hostile_doc, "device text injected a document key"
    assert hostile_doc["thermal_state"]["zones"][0]["zone"] == 'cpu"]},"pwned":true'
    assert run([sys.executable, "-B", str(VALIDATOR), "validate", str(SCHEMA),
                str(hostile_out)]).returncode == 0


def case_nominal(tmp: Path) -> None:
    out = tmp / "nominal.json"
    result = report(NOMINAL, out)
    assert result.returncode == 0, f"nominal must exit 0, got {result.returncode}"
    doc = load(out)
    assert doc["verdict"] == "PASS" and doc["blocked_on"] is None
    assert doc["crashes"] == 0
    assert doc["metrics"]["helper_rss_mb"]["value"] is None
    assert doc["metrics"]["helper_rss_mb"]["status"] == "NOT_REQUIRED"
    decode = doc["metrics"]["decode_p95_ms"]
    assert decode["value"] == 36.09 and decode["gated"] is False
    assert decode["status"] == "INFORMATIONAL" and "WITHDRAWN" in decode["note"]
    assert doc["provenance"]["package_digest"] is None
    assert doc["provenance"]["equivalence_status"] == "provisional"
    assert doc["thermal_state"]["peak_c"] == 52.0
    assert run([sys.executable, "-B", str(VALIDATOR), "validate", str(SCHEMA), str(out)]).returncode == 0

    cli_out = tmp / "cli-nominal.json"
    assert shell(["--mode", "wired", "--duration", "30m", "--capture", str(NOMINAL),
                  "--out", str(cli_out), "--now", NOW]).returncode == 0
    assert load(cli_out)["verdict"] == "PASS"
    assert run([sys.executable, "-B", str(VALIDATOR), "validate", str(SCHEMA),
                str(cli_out)]).returncode == 0


def case_budget_fail(tmp: Path) -> None:
    out = tmp / "overload.json"
    result = report(OVERLOAD, out)
    assert result.returncode == 1, f"overload must exit 1, got {result.returncode}"
    doc = load(out)
    assert doc["verdict"] == "FAIL"
    flagged = {v["metric"] for v in doc["violations"]}
    assert {"receiver_rss_peak_mb", "receiver_cpu_sustained_pct_one_core", "rss_growth_pct",
            "av_sync_ms", "audio_latency_ms", "crashes"} <= flagged, flagged
    assert run([sys.executable, "-B", str(VALIDATOR), "validate", str(SCHEMA), str(out)]).returncode == 0

    # Exact boundaries: <= budgets pass, over fails; RSS growth is strict (<10).
    boundaries = [
        ("rss_at_limit", dict(rss0=250.0, rss1=250.0, cpu=10.0, av=10.0, audio=10.0), 0),
        ("rss_over", dict(rss0=250.01, rss1=250.01, cpu=10.0, av=10.0, audio=10.0), 1),
        ("cpu_at_limit", dict(rss0=100.0, rss1=100.0, cpu=50.0, av=10.0, audio=10.0), 0),
        ("cpu_over", dict(rss0=100.0, rss1=100.0, cpu=50.01, av=10.0, audio=10.0), 1),
        ("av_at_limit", dict(rss0=100.0, rss1=100.0, cpu=10.0, av=50.0, audio=10.0), 0),
        ("av_over", dict(rss0=100.0, rss1=100.0, cpu=10.0, av=50.01, audio=10.0), 1),
        ("audio_at_limit", dict(rss0=100.0, rss1=100.0, cpu=10.0, av=10.0, audio=200.0), 0),
        ("audio_over", dict(rss0=100.0, rss1=100.0, cpu=10.0, av=10.0, audio=200.01), 1),
        ("growth_just_under", dict(rss0=100.0, rss1=109.99, cpu=10.0, av=10.0, audio=10.0), 0),
        ("growth_at_limit", dict(rss0=100.0, rss1=110.0, cpu=10.0, av=10.0, audio=10.0), 1),
        ("crash", dict(rss0=100.0, rss1=100.0, cpu=10.0, av=10.0, audio=10.0, crashes=1), 1),
    ]
    for name, kwargs, expected in boundaries:
        capture = make_capture(tmp, f"{name}.json", **kwargs)
        rc = report(capture, tmp / f"{name}-out.json").returncode
        assert rc == expected, f"{name}: expected exit {expected}, got {rc}"


def case_duration(tmp: Path) -> None:
    for bad in ["30x", "abc", "1.5m", "-5m", "m", ""]:
        out = tmp / "should-not-exist.json"
        res = shell(["--mode", "wired", "--duration", bad, "--capture", str(NOMINAL),
                     "--out", str(out)])
        assert res.returncode == 2, f"bad duration {bad!r} must be usage error, got {res.returncode}"
        assert "usage error" in res.stderr, f"bad duration {bad!r} produced no usage error"
        assert not out.exists(), f"bad duration {bad!r} wrote an output file"

    assert shell(["--mode", "wireless", "--duration", "30m", "--capture", str(NOMINAL),
                  "--out", str(tmp / "x.json")]).returncode == 2
    assert shell(["--mode", "wired", "--out", str(tmp / "y.json")]).returncode == 2
    assert shell(["--mode", "wired", "--duration", "30m"]).returncode == 2

    for good in ["30m", "2h", "90s", "45"]:
        out = tmp / f"dur-{good}.json"
        res = shell(["--mode", "wired", "--duration", good, "--capture", str(NOMINAL),
                     "--out", str(out), "--now", NOW])
        assert res.returncode == 0, f"valid duration {good!r} failed: {res.stderr}"
        assert load(out)["duration_requested"] == good


def case_contract(tmp: Path) -> None:
    base = load(NOMINAL)

    helper = tmp / "helper.json"
    doc = json.loads(json.dumps(base))
    doc["events"]["helper_rss_mb"] = 12.0
    write(helper, doc)
    assert report(helper, tmp / "o1.json").returncode == 4, "non-null helper RSS must be rejected"

    pkg = tmp / "pkg.json"
    doc = json.loads(json.dumps(base))
    doc["provenance"] = {"package_digest": "deadbeef"}
    write(pkg, doc)
    assert report(pkg, tmp / "o2.json").returncode == 4, "non-null package_digest must be rejected"

    eq = tmp / "eq.json"
    doc = json.loads(json.dumps(base))
    doc["provenance"] = {"equivalence_status": "established"}
    write(eq, doc)
    assert report(eq, tmp / "o3.json").returncode == 4, "non-provisional equivalence must be rejected"

    wrong_mode = tmp / "mode.json"
    doc = json.loads(json.dumps(base))
    doc["mode"] = "wireless"
    write(wrong_mode, doc)
    assert report(wrong_mode, tmp / "o4.json").returncode == 4, "non-wired capture must be rejected"

    broken = tmp / "broken.json"
    broken.write_text("{not json", encoding="utf-8")
    assert report(broken, tmp / "o5.json").returncode == 2, "malformed capture must be usage/IO error"

    array = tmp / "array.json"
    write(array, [1, 2, 3])
    assert report(array, tmp / "o6.json").returncode == 4, "non-object capture must be rejected"

    # stale_state: each run must produce a fresh document, never inherit a prior verdict.
    out = tmp / "stale.json"
    assert report(NOMINAL, out).returncode == 0 and load(out)["verdict"] == "PASS"
    assert report(OVERLOAD, out).returncode == 1 and load(out)["verdict"] == "FAIL"
    assert report(NOMINAL, out).returncode == 0 and load(out)["verdict"] == "PASS"

    assert shell(["--mode", "wired", "--duration", "30m", "--capture", str(NOMINAL),
                  "--out", str(out), "--now", NOW]).returncode == 0
    assert shell(["--mode", "wired", "--duration", "30m", "--capture", str(OVERLOAD),
                  "--out", str(out), "--now", NOW]).returncode == 1
    assert load(out)["verdict"] == "FAIL", "shell capture mode inherited a stale verdict"

    forced = report(OVERLOAD, tmp / "forced.json", ["--blocked-on", "readiness"])
    assert forced.returncode == 4, (
        "forcing BLOCKED over measured violations must be a contract violation, "
        f"got {forced.returncode}"
    )
    assert not (tmp / "forced.json").exists(), "violation-hiding document was written"


def _collect(tmp: Path, state_dir: Path, seconds: int, env: dict | None = None) -> subprocess.CompletedProcess:
    return run([sys.executable, "-B", str(COLLECTOR), "--receiver", str(FAKE_RECEIVER),
                "--port", "1-2.1", "--duration-seconds", str(seconds), "--interval", "1",
                "--no-display", "--state-dir", str(state_dir)], env=env, timeout=120)


def case_collector(tmp: Path) -> None:
    env = os.environ.copy()
    state = tmp / "state"
    res = _collect(tmp, state, 3, env)
    assert res.returncode == 0, f"collector failed: {res.stderr}"
    capture_path = tmp / "capture.json"
    capture_path.write_text(res.stdout, encoding="utf-8")
    capture = load(capture_path)
    assert capture["schema"] == "aa-projection-perf-capture/1"
    assert len(capture["samples"]) >= 2, capture["samples"]
    assert all(s["receiver_rss_mb"] and s["receiver_rss_mb"] > 0 for s in capture["samples"])
    assert capture["crashes"] == 0
    assert capture["events"]["decode_p95_ms"] == 25.0
    assert capture["events"]["video_units"] is not None
    assert "machine" in capture["target_hardware_state"]

    live_out = tmp / "live.json"
    assert report(capture_path, live_out).returncode == 3, "a live capture without gated metrics must BLOCK"
    assert load(live_out)["verdict"] == "BLOCKED"

    crash_env = os.environ.copy()
    crash_env["FAKE_RECEIVER_CRASH"] = "1"
    crash = _collect(tmp, tmp / "state-crash", 3, crash_env)
    crash_capture = load_from_stdout(crash.stdout)
    assert crash_capture["crashes"] >= 1, "a crashing receiver must be detected"

    grow_env = os.environ.copy()
    grow_env["FAKE_RECEIVER_GROW"] = "1"
    grow = _collect(tmp, tmp / "state-grow", 3, grow_env)
    assert grow.returncode == 0, f"collector failed on a growing process: {grow.stderr}"
    grow_capture = load_from_stdout(grow.stdout)
    assert grow_capture["crashes"] == 0 and grow_capture["samples"]
    assert grow_capture["events"]["helper_rss_mb"] is None

    # stale_state: reruns into the same state dir must not accumulate samples.
    first = _collect(tmp, state, 2, os.environ.copy())
    second = _collect(tmp, state, 2, os.environ.copy())
    first_n = len(load_from_stdout(first.stdout)["samples"])
    second_n = len(load_from_stdout(second.stdout)["samples"])
    assert second_n <= first_n + 1, (first_n, second_n)


def case_readiness(tmp: Path) -> None:
    out = tmp / "readiness.json"
    res = shell(["--readiness-only", "--host", "127.0.0.1", "--out", str(out)], timeout=45)
    assert res.returncode == 3, f"unmet prerequisites must exit 3, got {res.returncode}: {res.stderr}"
    doc = load(out)
    assert doc["schema"] == "aa-projection-perf-readiness/1"
    assert doc["ready"] is False and doc["blocked_on"], doc

    missing = shell(["--readiness-only", "--host", "127.0.0.1", "--out", str(tmp / "r.json"),
                     "--bogus"], timeout=45)
    assert missing.returncode == 2, "unknown argument must be a usage error"


def load_from_stdout(text: str) -> dict:
    return json.loads(text)


CASES = {
    "schema": case_schema,
    "nominal": case_nominal,
    "budget_fail": case_budget_fail,
    "duration": case_duration,
    "contract": case_contract,
    "collector": case_collector,
    "readiness": case_readiness,
}


def main(argv: list[str]) -> int:
    if len(argv) != 1 or argv[0] not in CASES:
        print(f"usage: test_wired_perf.py [{'|'.join(CASES)}]", file=sys.stderr)
        return 2
    case = argv[0]
    with tempfile.TemporaryDirectory(prefix="wired-perf-") as base:
        try:
            CASES[case](Path(base))
        except AssertionError as exc:
            print(f"FAIL [{case}]: {exc}", file=sys.stderr)
            return 1
    print(f"PASS [{case}]")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
