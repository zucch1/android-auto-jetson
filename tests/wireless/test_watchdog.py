# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/wireless/test_watchdog.py
"""Run the real watchdog with isolated command fixtures; never contact hardware."""
from pathlib import Path
import shutil
import signal
import subprocess
from tempfile import TemporaryDirectory
from typing import Final
import unittest

WATCHDOG: Final = Path(__file__).resolve().parents[2] / (
    'tools/jetson/rtl8822ce-regd-shim/rtl8822ce-regd-watchdog.sh')
MOCK: Final = r'''#!/bin/bash
set -euo pipefail
name=${0##*/}
printf '%s\n' "$name $*" >> "$FIXTURE/trace"
loads=0
if [[ -f $FIXTURE/loads ]]; then read -r loads < "$FIXTURE/loads"; fi
case "$name" in
  iw)
    if [[ $1 == dev ]]; then
      phy=${BEFORE_PHY:-0}
      status=${DEV_STATUS:-0}
      if (( loads > 0 )); then phy=${AFTER_PHY:-0}; status=${AFTER_DEV_STATUS:-0}; fi
      if [[ $phy != missing ]]; then printf 'Interface %s\n\twiphy %s\n' "$2" "$phy"; fi
      exit "$status"
    fi
    country=${BEFORE_COUNTRY:-00:}
    phy=${BEFORE_PHY:-0}
    status=${BEFORE_REG_STATUS:-0}
    if (( loads > 0 )); then
      country=${AFTER_COUNTRY:-US:}
      phy=${AFTER_PHY:-0}
      status=${AFTER_REG_STATUS:-0}
      if (( loads >= ${RECOVER_AT:-999} )); then country=US:; status=0; fi
    fi
    printf 'global\ncountry US: DFS-FCC\nphy#9 (self-managed)\ncountry US: DFS-FCC\n'
    if [[ $phy != missing ]]; then
      printf 'phy#%s (self-managed)\n' "$phy"
      if [[ $country != missing ]]; then printf 'country %s DFS-FCC\n' "$country"; fi
    fi
    # A stale wiphy can still be US while the newly resolved target is not.
    if [[ $phy != 0 ]]; then printf 'phy#0 (self-managed)\ncountry US: DFS-FCC\n'; fi
    if [[ ${REG_PADDING:-0} == 1 ]]; then printf '%0100000d\n' 0; fi
    exit "$status"
    ;;
  lsmod)
    if [[ ${PRESENT:-1} == 1 || $loads -gt 0 && ${LOAD_STATUS:-0} == 0 ]]; then
      printf 'rtl8822ce_regd_shim 16384 0\n'
      if [[ ${MODULE_PADDING:-0} == 1 ]]; then printf '%0100000d\n' 0; fi
    fi
    ;;
  rmmod) exit "${UNLOAD_STATUS:-0}" ;;
  modprobe)
    if [[ ${LOAD_STATUS:-0} != 0 ]]; then exit "$LOAD_STATUS"; fi
    printf '%s\n' "$((loads + 1))" > "$FIXTURE/loads"
    ;;
  logger) ;;
  sleep)
    polls=0
    if [[ -f $FIXTURE/polls ]]; then read -r polls < "$FIXTURE/polls"; fi
    printf '%s\n' "$((polls + 1))" > "$FIXTURE/polls"
    if (( polls + 1 >= ${POLLS:-1} )); then
      [[ $PPID == "$WATCHDOG_PID" && $PPID -gt 1 ]]
      kill -TERM "$PPID"
    fi
    ;;
  *) exit 99 ;;
esac
'''


class Watchdog(unittest.TestCase):
    def run_watchdog(self, settings: dict[str, str], iface: str = '') -> list[str]:
        """Each run owns its PATH, state and trace; only pure text tools are real."""
        with TemporaryDirectory(prefix='watchdog-') as directory:
            root = Path(directory)
            mock = root / 'mock'
            mock.write_text(MOCK)
            mock.chmod(0o700)
            for command in ('iw', 'lsmod', 'rmmod', 'modprobe', 'logger', 'sleep'):
                (root / command).symlink_to(mock)
            for command in ('sed', 'awk', 'grep'):
                executable = shutil.which(command)
                self.assertIsNotNone(executable)
                if executable is None:
                    self.fail(f'required text tool unavailable: {command}')
                (root / command).symlink_to(executable)
            env = dict(PATH=directory, FIXTURE=directory, **settings)
            argv = ['/bin/bash', '-c', 'export WATCHDOG_PID=$$; exec /bin/bash "$@"',
                    'watchdog-fixture', str(WATCHDOG)] + ([iface] if iface else [])
            result = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, -signal.SIGTERM, result.stderr)
            self.assertEqual(result.stderr, '')
            trace = (root / 'trace').read_text().splitlines()
            self.assertEqual([line for line in trace if line.startswith('sleep ')],
                             ['sleep 5'] * int(settings.get('POLLS', '1')))
            return trace

    def assert_verified(self, trace: list[str], present: bool) -> None:
        success = [i for i, line in enumerate(trace) if ' ok' in line]
        self.assertEqual(len(success), 1, trace)
        loaded = trace.index('modprobe rtl8822ce_regd_shim')
        self.assertIn('iw dev wlan-test info', trace[loaded + 1:success[0]])
        self.assertIn('iw reg get', trace[loaded + 1:success[0]])
        self.assertIn('country US:', trace[success[0]])
        self.assertEqual('rmmod rtl8822ce_regd_shim' in trace, present)

    def assert_retry(self, trace: list[str]) -> None:
        self.assertFalse(any(' ok' in line for line in trace), trace)
        self.assertTrue(any('WARN:' in line or 'ERROR:' in line for line in trace), trace)

    def test_already_us_skips_reload(self) -> None:
        # Given an already-correct target; when polled; then no loader is called.
        trace = self.run_watchdog({'BEFORE_COUNTRY': 'US:'})
        self.assertIn('iw dev wlP1p1s0 info', trace)
        self.assertFalse(any(line.startswith(('modprobe ', 'rmmod ', 'lsmod ')) for line in trace))

    def test_load_verifies_target_in_both_branches(self) -> None:
        for present in (False, True):
            with self.subTest(present=present):
                # Given drift; when loaded/reloaded; then only verified US is success.
                trace = self.run_watchdog({'PRESENT': str(int(present)), 'REG_PADDING': '1',
                                           'MODULE_PADDING': '1'}, 'wlan-test')
                self.assert_verified(trace, present)

    def test_failed_load_retries_in_both_branches(self) -> None:
        for present in (False, True):
            with self.subTest(present=present):
                # Given a loader failure; when two polls run; then both attempt recovery.
                trace = self.run_watchdog({'PRESENT': str(int(present)), 'LOAD_STATUS': '1', 'POLLS': '2'})
                self.assert_retry(trace)
                self.assertEqual(trace.count('modprobe rtl8822ce_regd_shim'), 2)

    def test_busy_unload_retries_without_loading(self) -> None:
        # Given a busy module; when two polls run; then unloading is retried safely.
        trace = self.run_watchdog({'UNLOAD_STATUS': '1', 'POLLS': '2'})
        self.assert_retry(trace)
        self.assertEqual(trace.count('rmmod rtl8822ce_regd_shim'), 2)
        self.assertNotIn('modprobe rtl8822ce_regd_shim', trace)

    def test_unresolved_interface_skips_load(self) -> None:
        for failure in ({'BEFORE_PHY': 'missing'}, {'DEV_STATUS': '1'}):
            with self.subTest(failure=failure):
                # Given no usable interface; when polled; then no other-phy fallback occurs.
                trace = self.run_watchdog(failure)
                self.assert_retry(trace)
                self.assertNotIn('modprobe rtl8822ce_regd_shim', trace)
                self.assertNotIn('iw reg get', trace)

    def test_failed_verification_retries_in_both_branches(self) -> None:
        failures = ({'AFTER_PHY': 'missing'}, {'AFTER_COUNTRY': '00:'},
                    {'AFTER_COUNTRY': 'missing'}, {'AFTER_COUNTRY': 'USA:'},
                    {'AFTER_COUNTRY': 'US'}, {'AFTER_REG_STATUS': '1'},
                    {'AFTER_DEV_STATUS': '1'}, {'AFTER_PHY': '2', 'AFTER_COUNTRY': '00:'})
        for present in (False, True):
            for failure in failures:
                with self.subTest(present=present, failure=failure):
                    # Given invalid evidence plus unrelated US; when loaded; then warn, not success.
                    trace = self.run_watchdog(dict(failure, PRESENT=str(int(present))))
                    self.assert_retry(trace)
                    self.assertIn('modprobe rtl8822ce_regd_shim', trace)
                    self.assertTrue(any('WARN:' in line for line in trace), trace)

    def test_failed_initial_iw_output_cannot_mask_drift(self) -> None:
        # Given plausible US on a failing iw; when polled; then recovery is attempted.
        trace = self.run_watchdog({'BEFORE_COUNTRY': 'US:', 'BEFORE_REG_STATUS': '1'}, 'wlan-test')
        self.assert_verified(trace, True)

    def test_new_wiphy_is_resolved_after_loading(self) -> None:
        for present in (False, True):
            with self.subTest(present=present):
                # Given renumbering on load; when recovering; then verify the new target.
                trace = self.run_watchdog({'PRESENT': str(int(present)), 'AFTER_PHY': '2'}, 'wlan-test')
                self.assert_verified(trace, present)

    def test_failed_verification_recovers_on_later_poll(self) -> None:
        for present in (False, True):
            with self.subTest(present=present):
                # Given first-load drift; when a second bounded poll runs; then retry succeeds.
                trace = self.run_watchdog({'PRESENT': str(int(present)), 'AFTER_COUNTRY': '00:',
                                           'RECOVER_AT': '2', 'POLLS': '2'})
                first_poll = trace.index('sleep 5')
                self.assert_retry(trace[:first_poll])
                self.assertEqual(trace.count('modprobe rtl8822ce_regd_shim'), 2)
                self.assertEqual(sum(' ok' in line for line in trace[first_poll + 1:]), 1)


if __name__ == '__main__':
    unittest.main()
