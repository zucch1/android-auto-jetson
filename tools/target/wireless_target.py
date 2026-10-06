# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# Run: streamed by probe-wireless.sh, never invoked by host tests against hardware.
"""Target capability discovery and fail-closed AP eligibility policy."""
import argparse
from dataclasses import asdict, dataclass, field
import json
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import uuid


@dataclass(slots=True)  # noqa: MUTABLE_OK - observations accumulate across probe phases
class Report:
    """Mutable accumulator of observations, not requested capability claims."""
    schema: str = 'aa-wireless-probe/1'
    target: str = ''
    phase: str = 'read-only'
    configured_jurisdiction: str = 'US'
    confirmed_jurisdiction: str = ''
    effective_regulatory_domain: str = ''
    ap_mode: bool = False
    channel_36_eligible: bool = False
    channel_36_operation: bool = False
    client_association: bool = False
    bluez_profile_capability: bool = False
    bluez_agent_capability: bool = False
    bluez_profile_registration: bool = False
    bluez_agent_registration: bool = False
    agent_callbacks: list[str] = field(default_factory=list)
    pairing_asserted: bool = False
    trust_asserted: bool = False
    phone_hosted_ap_or_p2p_asserted: bool = False
    target_qualified: bool = False
    blockers: list[str] = field(default_factory=list)
    raw: dict[str, str] = field(default_factory=dict)
    protected_inventory: dict[str, str] = field(default_factory=dict)


def command(args: list[str]) -> str:
    result = subprocess.run(args, capture_output=True, text=True, timeout=30, check=True)
    return result.stdout


def regulatory_section(reg: str, phy: str) -> str:
    """A self-managed phy overrides global rules; never combine other phys."""
    sections = re.split(r'(?m)^(global|phy#\d+[^\n]*)\n', reg)
    selected = ''
    for index in range(1, len(sections), 2):
        name, body = sections[index:index + 2]
        if name.split()[0] == 'phy#' + phy.removeprefix('phy'):
            return body
        if name == 'global':
            selected = body
    return selected


def eligible(info: str, reg: str, jurisdiction: str) -> bool:
    """Conservatively require an explicitly permitted 20 MHz AP channel."""
    channel = re.findall(r'(?m)^\s*\* 5180 MHz \[36\]([^\n]*)$', info)
    country = re.search(r'(?m)^country ([A-Z0-9]{2}):', reg)
    if (len(channel) != 1 or country is None or not jurisdiction or
            country.group(1) != jurisdiction or
            re.search(r'disabled|no[ -]IR|passive|radar|DFS|no 20MHz|no OFDM', channel[0], re.I)):
        return False
    for line in reg.splitlines():
        rule = re.search(r'\((\d+)\s*-\s*(\d+)\s*@\s*(\d+)\)', line)
        if rule and int(rule[1]) <= 5170 and int(rule[2]) >= 5190 and int(rule[3]) >= 20:
            if not re.search(r'NO.IR|DFS|PASSIVE|NO.OUTDOOR|NO.20MHZ|NO.OFDM', line, re.I):
                return True
    return False


def discover(report: Report, interface: str) -> str:
    phy = Path(f'/sys/class/net/{interface}/phy80211').resolve(strict=True).name
    driver = Path(f'/sys/class/net/{interface}/device/driver').resolve(strict=True).name
    report.raw['driver'] = driver
    if driver not in ('rtw_8822ce', 'rtw88_8822ce', 'rtl8822ce'):
        report.blockers.append('rtl8822ce-not-identified')
    report.raw['iw_phy'] = command(['iw', 'phy', phy, 'info'])
    report.raw['iw_reg'] = command(['iw', 'reg', 'get'])
    report.raw['nm'] = command(['nmcli', '-t', '-f', 'GENERAL.STATE,GENERAL.CONNECTION',
                                'device', 'show', interface])
    report.raw['bluez'] = command(['busctl', '--system', 'introspect', 'org.bluez', '/org/bluez'])
    section = regulatory_section(report.raw['iw_reg'], phy)
    country = re.search(r'(?m)^country ([A-Z0-9]{2}):', section)
    report.effective_regulatory_domain = country[1] if country else 'unknown'
    report.ap_mode = bool(re.search(r'(?m)^\s*\* AP\s*$', report.raw['iw_phy']))
    report.channel_36_eligible = eligible(report.raw['iw_phy'], section,
                                          report.confirmed_jurisdiction)
    report.bluez_profile_capability = 'org.bluez.ProfileManager1' in report.raw['bluez']
    report.bluez_agent_capability = 'org.bluez.AgentManager1' in report.raw['bluez']
    for passed, blocker in ((report.ap_mode, 'missing-ap-capability'),
                            (report.channel_36_eligible, 'channel-36-ineligible'),
                            (report.bluez_profile_capability, 'missing-bluez-profile-capability'),
                            (report.bluez_agent_capability, 'missing-bluez-agent-capability')):
        if not passed:
            report.blockers.append(blocker)
    if not report.confirmed_jurisdiction:
        report.blockers.append('jurisdiction-unconfirmed')
    return phy


def active(report: Report, args: argparse.Namespace) -> None:
    """Create only an in-memory NM profile; never change regulatory state."""
    if report.blockers:
        return
    if not re.search(r'(?m)^GENERAL.STATE:30(?:\s|$)', report.raw['nm']):
        report.blockers.append('interface-not-isolated-disconnected')
        return
    # These bindings must already exist on the target. No installation is attempted.
    import importlib
    bluez = importlib.import_module('wireless_bluez')
    connection = str(uuid.uuid4())
    report.raw['connection_uuid'] = connection
    report.phase = 'active'
    try:
        command(['nmcli', 'connection', 'add', 'save', 'no', 'type', 'wifi',
                 'ifname', args.interface, 'con-name', 'aa-task12-' + connection,
                 'connection.uuid', connection, 'connection.autoconnect', 'no',
                 'ssid', 'aa-task12-isolated', '802-11-wireless.mode', 'ap',
                 '802-11-wireless.band', 'a', '802-11-wireless.channel', '36',
                 '802-11-wireless-security.key-mgmt', 'wpa-psk',
                 '802-11-wireless-security.psk', args.ap_psk,
                 'ipv4.method', 'disabled', 'ipv6.method', 'disabled'])
        command(['nmcli', '--wait', '20', 'connection', 'up', 'uuid', connection])
        report.raw['iw_operation'] = command(['iw', 'dev', args.interface, 'info'])
        report.channel_36_operation = bool(re.search(
            r'channel 36 \(5180 MHz\)', report.raw['iw_operation'])) and 'type AP' in report.raw['iw_operation']
        if not report.channel_36_operation:
            report.blockers.append('channel-36-operation-failed')
            return
        with bluez.registration(report) as context:
            end = time.monotonic() + args.duration
            while time.monotonic() < end:
                context.iteration(False)
                stations = command(['iw', 'dev', args.interface, 'station', 'dump'])
                report.raw['stations'] = stations
                report.client_association |= bool(re.search(
                    r'(?ms)^Station [0-9a-f:]{17} [^\n]*\n(?:(?!^Station ).)*?\bauthorized:\s+yes\b',
                    stations))
                time.sleep(0.1)
        if not report.client_association:
            report.blockers.append('client-association-not-observed')
    finally:
        # UUID is unique to this run; never delete another connection.
        try:
            command(['nmcli', 'connection', 'delete', 'uuid', connection])
        except (subprocess.SubprocessError, OSError) as error:
            report.blockers.append('ap-cleanup-failed')
            report.raw['cleanup_error'] = str(error)


def main() -> int:
    def interrupted(signum: int, frame) -> None:
        raise InterruptedError(signum)
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, interrupted)
    parser = argparse.ArgumentParser()
    parser.add_argument('--target', required=True)
    parser.add_argument('--interface', required=True)
    parser.add_argument('--jurisdiction-confirm', default='')
    parser.add_argument('--active', action='store_true')
    parser.add_argument('--duration', type=int, default=30)
    parser.add_argument('--ap-psk', default='')
    args = parser.parse_args()
    report = Report(target=args.target, confirmed_jurisdiction=args.jurisdiction_confirm)
    try:
        discover(report, args.interface)
        if args.active:
            active(report, args)
    except (OSError, subprocess.SubprocessError, ImportError) as error:
        report.blockers.append('capability-operation-failed')
        report.raw['error_type'] = type(error).__name__
    print(json.dumps(asdict(report), sort_keys=True))
    return int(bool(report.blockers))


if __name__ == '__main__':
    sys.exit(main())
