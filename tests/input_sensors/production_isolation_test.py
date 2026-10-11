#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Production-isolation check for the task-26 simulated sensor fixture (gate r2).

The fixture enablement + synthetic-emission API must be reachable ONLY from
test builds. This check proves structural isolation, not marker discipline:

1. positive control: a TU using the fixture API compiles when the TEST-ONLY
   header is on the include path (the API name is real, so the negatives are
   not vacuous);
2. compile-fail: the same TU against the PRODUCTION include dir alone must
   fail - the production headers declare no fixture API;
3. link-availability: a consumer links against the test-support archive but
   fails with undefined references against the production archive alone;
4. symbol inventory: the production archive carries NO fixture symbols while
   the test-support archive carries them;
5. source inventory: production input/sensor sources and headers contain no
   fixture API names.

Usage: production_isolation_test.py CXX INCLUDE TESTS_DIR PROD_LIB FIXTURE_LIB
Exit: 0 isolated, 1 violation, 2 usage error.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

FIXTURE_NAMES = (
    "SimulatedSensorFixture",
    "SimulatedSensorEvent",
    "TestOnlySensorFixture",
    "enable_simulated_fixtures",
    "emit_simulated",
)

PRODUCTION_TU = """\
#include <aa/input/Capability.hpp>
#include <aa/input/InputService.hpp>
#include <aa/input/SensorService.hpp>
#ifdef AA_ISOLATION_WITH_TEST_HEADER
#include "SimulatedSensorFixture.hpp"
#endif

struct Target final : aa::input::SensorChannelTarget {
    void on_sensor_event(const aa::input::SensorEvent&) override {}
};

int main() {
    Target target;
    inputsensorstest::SimulatedSensorFixture fixture{target};
    const aa::input::SensorChannel channels[] = {aa::input::SensorChannel::speed};
    fixture.enable(channels);
    const auto outcome = fixture.emit(
        inputsensorstest::SimulatedSensorEvent{aa::input::SensorChannel::speed, 1});
    return outcome ? 0 : 1;
}
"""


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, check=False)


def main(argv: list[str]) -> int:
    if len(argv) != 6:
        print("usage: production_isolation_test.py CXX INCLUDE TESTS_DIR PROD_LIB FIXTURE_LIB")
        return 2
    cxx, include, tests_dir, prod_lib, fixture_lib = argv[1:]
    for path in (include, tests_dir, prod_lib, fixture_lib):
        if not Path(path).exists():
            print(f"missing input: {path}")
            return 2

    with tempfile.TemporaryDirectory(prefix="aa-input-isolation-") as scratch:
        tu = Path(scratch) / "consumer.cpp"
        tu.write_text(PRODUCTION_TU, encoding="utf-8")

        # Positive control: the fixture API resolves with the test-only header.
        positive = run([cxx, "-std=c++20", "-fsyntax-only", f"-I{include}", f"-I{tests_dir}",
                        "-DAA_ISOLATION_WITH_TEST_HEADER", str(tu)])
        if positive.returncode != 0:
            print(f"positive control failed (fixture API unusable even in tests): "
                  f"{positive.stdout}{positive.stderr}")
            return 1

        # Compile-fail: production headers alone must not declare the API.
        negative = run([cxx, "-std=c++20", "-fsyntax-only", f"-I{include}", str(tu)])
        if negative.returncode == 0:
            print("production include dir compiled the fixture API - isolation broken")
            return 1

        # Link probes: the same consumer must resolve only with test support.
        fixture_link = run([cxx, "-std=c++20", f"-I{include}", f"-I{tests_dir}",
                            "-DAA_ISOLATION_WITH_TEST_HEADER", str(tu), fixture_lib,
                            "-o", str(Path(scratch) / "fixture-consumer")])
        if fixture_link.returncode != 0:
            print(f"fixture archive link failed: {fixture_link.stdout}{fixture_link.stderr}")
            return 1

        production_link = run([cxx, "-std=c++20", f"-I{include}", f"-I{tests_dir}",
                               "-DAA_ISOLATION_WITH_TEST_HEADER", str(tu), prod_lib,
                               "-o", str(Path(scratch) / "production-consumer")])
        if production_link.returncode == 0:
            print("production-only archive linked the fixture API - isolation broken")
            return 1
        if "undefined reference" not in production_link.stderr:
            print(f"production-only link failed for a reason other than undefined references: "
                  f"{production_link.stdout}{production_link.stderr}")
            return 1

        # Link-availability: no fixture symbols in the production archive.
        for archive, want in ((prod_lib, False), (fixture_lib, True)):
            listing = run(["nm", "-C", archive])
            if listing.returncode != 0:
                print(f"nm failed on {archive}: {listing.stderr}")
                return 1
            hits = [name for name in FIXTURE_NAMES if name in listing.stdout]
            if want and "SimulatedSensorFixture" not in hits:
                print(f"{archive} lacks the fixture symbols - check is vacuous: {hits}")
                return 1
            if not want and hits:
                print(f"{archive} exposes fixture API symbols: {hits}")
                return 1

    # Source inventory: production trees mention no fixture API name.
    source_root = Path(include).parent
    for root in (Path(include) / "aa" / "input", source_root / "src" / "input"):
        for source in sorted(root.rglob("*")):
            if source.suffix not in {".hpp", ".cpp"}:
                continue
            text = source.read_text(encoding="utf-8")
            hits = [name for name in FIXTURE_NAMES if name in text]
            if hits:
                print(f"{source} references fixture API names: {hits}")
                return 1

    print("production isolation confirmed: fixture API absent from production "
          "headers, sources and archive; present only in the test-support archive")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
