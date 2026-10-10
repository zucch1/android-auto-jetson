# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/usb/test_probe.py [selection|handshake|libusb|sysfs|hostenv|cycles|cli]
"""Hardware boundaries are faked: this suite never opens a real USB device."""
import ctypes
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/target"))
import usb_aoa_hostenv as hostenv  # noqa: E402
import usb_aoa_libusb as libusb  # noqa: E402
import usb_aoa_probe as host  # noqa: E402
import usb_aoa_sysfs as sysfs  # noqa: E402
import usb_aoa_target as target  # noqa: E402

APPROVED = ("04e8", "6860")
AOAP = ("18d1", "2d00")
SEND_LENGTHS = [8, 13, 13, 6, 22, 13]


def write_port(root: Path, port: str, vendor: str, product: str) -> None:
    base = root / port
    base.mkdir(parents=True, exist_ok=True)
    (base / "idVendor").write_text(vendor)
    (base / "idProduct").write_text(product)
    (base / "busnum").write_text("1")
    (base / "devnum").write_text("7")
    (base / "bMaxPower").write_text("500mA")
    (base / "manufacturer").write_text("SAMSUNG")
    (base / "product").write_text("SAMSUNG_Android")
    (base / "serial").write_text("TEST-SERIAL-001")
    (base / "bDeviceClass").write_text("00")
    interface = root / f"{port}:1.0"
    interface.mkdir(exist_ok=True)
    for name, address in (("ep_81", "81"), ("ep_01", "01")):
        endpoint = interface / name
        endpoint.mkdir(exist_ok=True)
        (endpoint / "bEndpointAddress").write_text(address)
        (endpoint / "bmAttributes").write_text("02")
        (endpoint / "wMaxPacketSize").write_text("0200")


def rewrite_ids(root: Path, port: str, vendor: str, product: str) -> None:
    (root / port / "idVendor").write_text(vendor)
    (root / port / "idProduct").write_text(product)


class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class FakeRunner:
    def __init__(self, state: str = "active", journal: str = "m1\nm2\n") -> None:
        self.state = state
        self.journal = journal
        self.calls: list[tuple[str, ...]] = []

    def run(self, args, timeout_s):
        self.calls.append(tuple(args))
        if args[0] == "systemctl":
            return hostenv.CommandResult(tuple(args), 0, self.state + "\n", "")
        return hostenv.CommandResult(tuple(args), 0, self.journal, "")


class FakeTransport:
    """Narrow transport double: records exact requests, never touches hardware."""

    def __init__(self, protocol: bytes = b"\x02\x00", short_string: int | None = None,
                 raise_on: tuple[str, ...] = ()) -> None:
        self.calls: list[tuple[str, int, int, int, int, int]] = []
        self.send_payloads: list[bytes] = []
        self.closed = False
        self.resets = 0
        self._protocol = protocol
        self._short = short_string
        self._raise = set(raise_on)

    def control_in(self, request, value, index, length, timeout_ms):
        self.calls.append(("in", request, value, index, length, timeout_ms))
        if "in" in self._raise:
            raise libusb.LibusbError("control-in", -7, "timeout")
        return self._protocol

    def control_out(self, request, value, index, payload, timeout_ms):
        self.calls.append(("out", request, value, index, len(payload), timeout_ms))
        if request == libusb.SEND_STRING:
            self.send_payloads.append(payload)
        if "out" in self._raise:
            raise libusb.LibusbError("control-out", -7, "timeout")
        if request == libusb.START:
            return 0
        if self._short is not None and index == self._short:
            return len(payload) - 1
        return len(payload)

    def reset(self):
        self.resets += 1

    def close(self):
        self.closed = True

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def make_env(root: Path, factory, clock: FakeClock | None = None, runner=None):
    clock = clock or FakeClock()
    return target.ProbeEnvironment(
        sysfs=sysfs.Sysfs(root), transport_factory=factory, runner=runner or FakeRunner(),
        udc_root=root / "udc", configfs_root=root / "configfs",
        clock=clock, sleep=clock.sleep)


def approved_options(cycles=0, duration=0.0):
    return target.Options("jetson.local", "1-2.1", "04e8:6860", (0x04E8, 0x6860),
                          cycles, duration, 1.0)


class Selection(unittest.TestCase):
    def test_unknown_device_is_unqualified(self):
        device = sysfs.PortDevice("1-2.1", 1, 7, 0x1234, 0x5678, "", "", "", 100, 0, ())
        selection = target.select(device, (0x04E8, 0x6860))
        self.assertTrue(selection.unqualified)
        self.assertFalse(selection.approved or selection.already_aoa)

    def test_approved_and_already_aoa(self):
        approved = sysfs.PortDevice("1-2.1", 1, 7, 0x04E8, 0x6860, "", "", "", 500, 0, ())
        self.assertTrue(target.select(approved, (0x04E8, 0x6860)).approved)
        for pid in (0x2D00, 0x2D01):
            aoa = sysfs.PortDevice("1-2.1", 1, 7, 0x18D1, pid, "", "", "", 500, 0, ())
            self.assertTrue(target.select(aoa, (0x04E8, 0x6860)).already_aoa)

    def test_unknown_device_makes_zero_requests(self):
        with TemporaryDirectory() as tmp:
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", "1234", "5678")
            calls = []
            report = target.run(approved_options(), make_env(root, lambda *a: calls.append(a)))
            self.assertEqual(report.blockers, ["unqualified-candidate"])
            self.assertFalse(report.qualified)
            self.assertEqual(calls, [])

    def test_wrong_port_is_not_a_pass(self):
        with TemporaryDirectory() as tmp:
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", *APPROVED)
            calls = []
            options = target.Options("jetson.local", "1-9.9", "04e8:6860", (0x04E8, 0x6860), 0, 0.0, 1.0)
            report = target.run(options, make_env(root, lambda *a: calls.append(a)))
            self.assertEqual(report.blockers, ["port-empty"])
            self.assertFalse(report.qualified)
            self.assertEqual(calls, [])

    def test_already_aoa_qualifies_without_requests(self):
        with TemporaryDirectory() as tmp:
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", *AOAP)
            calls = []
            report = target.run(approved_options(), make_env(root, lambda *a: calls.append(a)))
            self.assertTrue(report.qualified)
            self.assertEqual(report.phase, "already-aoa")
            self.assertEqual(report.blockers, [])
            self.assertEqual(calls, [])


class Handshake(unittest.TestCase):
    def test_exact_requests_and_lengths(self):
        transport = FakeTransport()
        result = libusb.handshake(transport)
        self.assertTrue(result.started)
        self.assertEqual(result.protocol_version, 2)
        self.assertEqual(result.requests, 8)
        self.assertEqual(transport.calls[0], ("in", 51, 0, 0, 2, 2000))
        outgoing = [call for call in transport.calls if call[0] == "out"]
        self.assertEqual([call[1] for call in outgoing], [52, 52, 52, 52, 52, 52, 53])
        self.assertEqual([call[2] for call in outgoing[:-1]], [0, 0, 0, 0, 0, 0])
        self.assertEqual([call[3] for call in outgoing[:-1]], [0, 1, 2, 3, 4, 5])
        self.assertEqual([(step.value, step.index) for step in result.steps],
                         [(0, 0), (0, 1), (0, 2), (0, 3), (0, 4), (0, 5)])
        self.assertEqual([call[4] for call in outgoing[:-1]], SEND_LENGTHS)
        self.assertEqual(outgoing[-1], ("out", 53, 0, 0, 0, 2000))
        for index, (_, _, text) in enumerate(libusb.SEND_STRINGS):
            self.assertEqual(transport.send_payloads[index], text.encode("ascii") + b"\x00")

    def test_short_get_protocol_is_rejected(self):
        transport = FakeTransport(protocol=b"\x02")
        result = libusb.handshake(transport)
        self.assertFalse(result.started)
        self.assertEqual(result.failed_at, "get-protocol-length")
        self.assertEqual(len(transport.calls), 1)

    def test_version_below_one_is_rejected(self):
        result = libusb.handshake(FakeTransport(protocol=b"\x00\x00"))
        self.assertEqual(result.failed_at, "get-protocol-version")
        self.assertFalse(result.started)

    def test_short_send_string_never_starts(self):
        transport = FakeTransport(short_string=0)
        result = libusb.handshake(transport)
        self.assertFalse(result.started)
        self.assertEqual(result.failed_at, "send-string-manufacturer")
        self.assertEqual(result.steps[0].observed_length, SEND_LENGTHS[0] - 1)
        self.assertFalse(any(call[0] == "out" and call[1] == 53 for call in transport.calls))

    def test_each_short_string_is_rejected_when_its_index_is_selected(self):
        for index, name in enumerate(("manufacturer", "model", "description", "version", "uri", "serial")):
            with self.subTest(index=index):
                # Given
                transport = FakeTransport(short_string=index)
                # When
                result = libusb.handshake(transport)
                # Then
                self.assertFalse(result.started)
                self.assertEqual(result.failed_at, f"send-string-{name}")
                self.assertFalse(any(call[1] == 53 for call in transport.calls))


class Libusb(unittest.TestCase):
    class ControlLib:
        def __init__(self, result: int, reply: bytes = b"") -> None:
            self.args: list[tuple] = []
            self.closed: list = []
            self._reply = reply

            def transfer(handle, rtype, request, value, index, data, length, timeout):
                self.args.append((rtype, request, value, index, length, timeout))
                if self._reply and data is not None:
                    ctypes.memmove(data, self._reply, len(self._reply))
                    return len(self._reply)
                return result

            self.libusb_control_transfer = transfer
            self.libusb_reset_device = lambda handle: 0
            self.libusb_close = lambda handle: self.closed.append(handle)

    def test_control_in_reads_exact_two_bytes(self):
        library = self.ControlLib(0, reply=b"\x02\x00")
        device = libusb.LibusbDevice(library, ctypes.c_void_p(1))
        self.assertEqual(device.control_in(51, 0, 0, 2, 2000), b"\x02\x00")
        self.assertEqual(library.args[0], (0xC0, 51, 0, 0, 2, 2000))

    def test_control_out_encodes_vendor_out_length(self):
        library = self.ControlLib(0)
        device = libusb.LibusbDevice(library, ctypes.c_void_p(1))
        self.assertEqual(device.control_out(52, 0, 0, b"Android\x00", 2000), 0)
        self.assertEqual(library.args[0], (0x40, 52, 0, 0, 8, 2000))

    def test_negative_transfer_raises_and_closes(self):
        library = self.ControlLib(-7)
        device = libusb.LibusbDevice(library, ctypes.c_void_p(1))
        with self.assertRaises(libusb.LibusbError) as caught:
            device.control_in(51, 0, 0, 2, 2000)
        self.assertEqual(caught.exception.code, -7)
        device.close()
        self.assertTrue(library.closed)
        device.close()
        self.assertEqual(len(library.closed), 1)

    def _open_lib(self, init_result: int, device_count: int):
        library = SimpleNamespace()
        library.libusb_init = lambda ctx: init_result
        library.libusb_exit = lambda ctx: None
        library.libusb_get_device_list = lambda ctx, out: device_count
        library.libusb_free_device_list = lambda listing, unref: None
        library.libusb_get_bus_number = lambda device: 0
        library.libusb_get_device_address = lambda device: 0
        library.libusb_get_device_descriptor = lambda device, desc: 0
        library.libusb_open = lambda device, handle: 0
        library.libusb_close = lambda handle: None
        library.libusb_control_transfer = lambda *args: 0
        library.libusb_reset_device = lambda handle: 0
        return library

    def test_init_failure_is_typed(self):
        with self.assertRaises(libusb.LibusbError) as caught:
            libusb.Libusb(self._open_lib(-1, 0))
        self.assertEqual(caught.exception.operation, "libusb_init")

    def test_missing_device_is_typed(self):
        library = libusb.Libusb(self._open_lib(0, 0))
        with self.assertRaises(libusb.LibusbError) as caught:
            library.open(1, 7, 0x04E8, 0x6860)
        self.assertEqual(caught.exception.code, -4)
        library.close()
        library.close()


class SysfsCase(unittest.TestCase):
    def test_scan_reads_descriptors_endpoints_and_power(self):
        with TemporaryDirectory() as tmp:
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", *APPROVED)
            device = sysfs.Sysfs(root).scan("1-2.1")
            self.assertEqual((device.vendor_id, device.product_id), (0x04E8, 0x6860))
            self.assertEqual(device.advertised_power_ma, 500)
            self.assertEqual(device.busnum, 1)
            self.assertEqual(device.devnum, 7)
            self.assertEqual({(ep.address, ep.direction, ep.max_packet_size) for ep in device.endpoints},
                             {("81", "in", 512), ("01", "out", 512)})
            self.assertEqual(device.endpoints[0].transfer_type, "bulk")

    def test_invalid_port_and_phone_id_are_rejected(self):
        with self.assertRaises(sysfs.SysfsError):
            sysfs.Sysfs(Path("/nonexistent")).scan("../../etc")
        with self.assertRaises(sysfs.SysfsError):
            sysfs.parse_phone_id("zz")
        self.assertEqual(sysfs.parse_phone_id("04e8:6860"), (0x04E8, 0x6860))

    def test_presence_reports_advertised_not_measured(self):
        with TemporaryDirectory() as tmp:
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", *APPROVED)
            sample = sysfs.Sysfs(root).presence("1-2.1")
            self.assertEqual((sample.vendor_id, sample.advertised_power_ma), (0x04E8, 500))


class HostenvCase(unittest.TestCase):
    def test_configured_idle_is_not_active(self):
        with TemporaryDirectory() as tmp:
            udc_root = Path(tmp) / "udc"
            (udc_root / "3550000.usb").mkdir(parents=True)
            (udc_root / "3550000.usb" / "state").write_text("default")
            configfs = Path(tmp) / "configfs"
            (configfs / "l4t").mkdir(parents=True)
            (configfs / "l4t" / "UDC").write_text("3550000.usb")
            status = hostenv.gadget_status(udc_root, configfs)
            self.assertTrue(status.configured)
            self.assertFalse(status.active)
            self.assertEqual(status.state, "default")
            self.assertEqual(status.udcs[0].bound_gadget, "l4t")

    def test_configured_state_is_active(self):
        with TemporaryDirectory() as tmp:
            udc_root = Path(tmp) / "udc"
            (udc_root / "3550000.usb").mkdir(parents=True)
            (udc_root / "3550000.usb" / "state").write_text("configured")
            configfs = Path(tmp) / "configfs"
            (configfs / "l4t").mkdir(parents=True)
            (configfs / "l4t" / "UDC").write_text("3550000.usb")
            self.assertTrue(hostenv.gadget_status(udc_root, configfs).active)

    def test_unbound_udc_is_not_configured(self):
        with TemporaryDirectory() as tmp:
            udc_root = Path(tmp) / "udc"
            (udc_root / "3550000.usb").mkdir(parents=True)
            (udc_root / "3550000.usb" / "state").write_text("not attached")
            self.assertFalse(hostenv.gadget_status(udc_root, Path(tmp) / "configfs").configured)

    def test_host_environment_is_presence_only(self):
        runner = FakeRunner(journal="a\n\nb\n")
        environment = hostenv.host_environment(runner, which=lambda name: "/usr/bin/adb" if name == "adb" else None)
        self.assertTrue(environment.modemmanager_active)
        self.assertEqual(environment.journal_lines, 2)
        self.assertTrue(environment.adb_present)
        self.assertFalse(environment.mmcli_present)
        self.assertEqual(runner.calls[0], ("systemctl", "is-active", "ModemManager"))


class Cycles(unittest.TestCase):
    def test_switch_then_software_cycle_reestablishes_aoa(self):
        with TemporaryDirectory() as tmp:
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", *APPROVED)
            transports = []

            def factory(bus, address, vendor, product):
                transport = FakeTransport()
                control_out = transport.control_out

                def switch(request, value, index, payload, timeout_ms):
                    result = control_out(request, value, index, payload, timeout_ms)
                    if request == libusb.START:
                        rewrite_ids(root, "1-2.1", *AOAP)
                    return result

                transport.control_out = switch

                def reset():
                    transport.resets += 1
                    rewrite_ids(root, "1-2.1", *AOAP)

                transport.reset = reset
                transports.append(transport)
                return transport

            report = target.run(approved_options(cycles=1), make_env(root, factory))
            self.assertEqual(report.blockers, [])
            self.assertTrue(report.qualified)
            self.assertEqual(len(transports), 2)
            self.assertEqual(transports[0].resets, 0)
            self.assertEqual(transports[1].resets, 1)
            self.assertTrue(report.cycles[0].reenumerated)
            self.assertTrue(report.cycles[0].reestablished_aoa)

    def test_timeout_cleanup_and_blocker(self):
        with TemporaryDirectory() as tmp:
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", *APPROVED)
            transport = FakeTransport(raise_on=("out",))
            report = target.run(approved_options(), make_env(root, lambda *a: transport))
            self.assertIn("aoa-handshake-failed", report.blockers)
            self.assertFalse(report.qualified)
            self.assertTrue(transport.closed)

    def test_incomplete_handshake_never_qualifies(self):
        for transport in (FakeTransport(protocol=b"\x02"), FakeTransport(short_string=3)):
            with self.subTest(transport=transport), TemporaryDirectory() as tmp:
                # Given
                root = Path(tmp) / "usb"
                write_port(root, "1-2.1", *APPROVED)
                # When
                report = target.run(approved_options(), make_env(root, lambda *a: transport))
                # Then
                self.assertFalse(report.qualified)
                self.assertIn("aoa-switch-incomplete", report.blockers)
                self.assertTrue(transport.closed)

    def test_presence_sampling_is_bounded(self):
        with TemporaryDirectory() as tmp:
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", *AOAP)
            clock = FakeClock()
            report = target.run(approved_options(duration=2.0), make_env(root, lambda *a: None, clock))
            self.assertTrue(report.presence.stable)
            self.assertEqual(report.presence.samples, 3)
            self.assertEqual(report.presence.advertised_power_ma, 500)

    def test_switch_stays_unqualified_when_same_port_never_enumerates_aoa(self):
        for other_port_aoa in (False, True):
            with self.subTest(other_port_aoa=other_port_aoa), TemporaryDirectory() as tmp:
                # Given: START succeeds, but the selected port keeps its phone IDs.
                root = Path(tmp) / "usb"
                write_port(root, "1-2.1", *APPROVED)
                if other_port_aoa:
                    write_port(root, "1-2.2", *AOAP)
                transport = FakeTransport()
                clock = FakeClock()
                # When
                report = target.run(approved_options(), make_env(root, lambda *a: transport, clock))
                # Then
                self.assertTrue(report.aoa.started)
                self.assertFalse(report.qualified)
                self.assertIn("aoa-reenumeration-timeout", report.blockers)
                self.assertEqual(clock.now, libusb.REENUMERATE_TIMEOUT_S)

    def test_switch_updates_device_when_same_port_enumerates_aoa(self):
        with TemporaryDirectory() as tmp:
            # Given: enumeration occurs on a later poll, not at START completion.
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", *APPROVED)
            clock = FakeClock()
            env = make_env(root, lambda *a: FakeTransport(), clock)

            def enumerate_aoa(seconds):
                clock.sleep(seconds)
                rewrite_ids(root, "1-2.1", *AOAP)
                (root / "1-2.1" / "devnum").write_text("8")

            env = target.ProbeEnvironment(env.sysfs, env.transport_factory, env.runner,
                                          env.udc_root, env.configfs_root, clock, enumerate_aoa)
            # When
            report = target.run(approved_options(), env)
            # Then
            self.assertTrue(report.qualified)
            self.assertEqual((report.device.vendor_id, report.device.product_id, report.device.devnum),
                             (0x18D1, 0x2D00, 8))
            self.assertEqual(report.presence.samples, 1)

    def test_aoa_stays_unqualified_when_bulk_pair_is_unusable(self):
        for initial_ids in (APPROVED, AOAP):
            for endpoint, attribute, value in (("ep_81", "bmAttributes", "03"),
                                                ("ep_01", "bmAttributes", "03"),
                                                ("ep_81", "wMaxPacketSize", "0000"),
                                                ("ep_01", "wMaxPacketSize", "bad-packet"),
                                                ("ep_81", "wMaxPacketSize", None),
                                                ("ep_01", "wMaxPacketSize", None)):
                with self.subTest(ids=initial_ids, endpoint=endpoint, attribute=attribute), TemporaryDirectory() as tmp:
                    # Given
                    root = Path(tmp) / "usb"
                    write_port(root, "1-2.1", *initial_ids)
                    attribute_path = root / "1-2.1:1.0" / endpoint / attribute
                    if value is None:
                        attribute_path.unlink()
                    else:
                        attribute_path.write_text(value)
                    transport = FakeTransport()
                    control_out = transport.control_out

                    def switch(request, value, index, payload, timeout_ms):
                        result = control_out(request, value, index, payload, timeout_ms)
                        if request == libusb.START:
                            rewrite_ids(root, "1-2.1", *AOAP)
                        return result

                    transport.control_out = switch
                    # When
                    report = target.run(approved_options(), make_env(root, lambda *a: transport))
                    # Then
                    self.assertFalse(report.qualified)
                    self.assertIn("aoa-bulk-endpoints-unusable", report.blockers)
                    self.assertEqual(transport.resets, 0)

    def test_unknown_cycle_rescan_performs_zero_usb_operations(self):
        with TemporaryDirectory() as tmp:
            # Given
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", "1234", "5678")
            opened = []

            def factory(*args):
                opened.append(args)
                return FakeTransport()

            # When
            outcome = target.cycle(approved_options(), make_env(root, factory), 0)
            # Then
            self.assertFalse(outcome.reset_ok or outcome.reestablished_aoa)
            self.assertEqual(opened, [])

    def test_all_requested_resets_succeed_before_qualification(self):
        with TemporaryDirectory() as tmp:
            # Given: an approved selection using AOA IDs must not trigger another START.
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", *AOAP)
            transport = FakeTransport()
            options = target.Options("jetson.local", "1-2.1", "18d1:2d00", (0x18D1, 0x2D00),
                                     3, 0.0, 1.0)
            # When
            report = target.run(options, make_env(root, lambda *a: transport))
            # Then
            self.assertTrue(report.qualified)
            self.assertEqual(transport.resets, 3)
            self.assertEqual(len(report.cycles), 3)
            self.assertTrue(all(cycle.reset_ok and cycle.reestablished_aoa for cycle in report.cycles))
            self.assertEqual(transport.calls, [])

    def test_cycle_restart_requires_actual_aoa_enumeration(self):
        for switch_to_aoa in (False, True):
            with self.subTest(switch_to_aoa=switch_to_aoa), TemporaryDirectory() as tmp:
                # Given: reset returns the phone to its original IDs.
                root = Path(tmp) / "usb"
                write_port(root, "1-2.1", *AOAP)
                transports = []

                def factory(*args):
                    transport = FakeTransport()
                    control_out = transport.control_out

                    def reset():
                        transport.resets += 1
                        rewrite_ids(root, "1-2.1", *APPROVED)

                    def switch(request, value, index, payload, timeout_ms):
                        result = control_out(request, value, index, payload, timeout_ms)
                        if request == libusb.START and switch_to_aoa:
                            rewrite_ids(root, "1-2.1", *AOAP)
                        return result

                    transport.reset = reset
                    transport.control_out = switch
                    transports.append(transport)
                    return transport

                # When
                report = target.run(approved_options(cycles=1), make_env(root, factory))
                # Then
                self.assertEqual(report.qualified, switch_to_aoa)
                self.assertEqual(report.cycles[0].reestablished_aoa, switch_to_aoa)
                if switch_to_aoa:
                    self.assertEqual((report.cycles[0].vendor_id, report.cycles[0].product_id),
                                     (0x18D1, 0x2D00))
                self.assertTrue(all(transport.closed for transport in transports))

    def test_cycle_failure_prevents_qualification(self):
        for failure in ("reset", "disappear", "endpoints"):
            with self.subTest(failure=failure), TemporaryDirectory() as tmp:
                # Given
                root = Path(tmp) / "usb"
                write_port(root, "1-2.1", *AOAP)
                transport = FakeTransport()

                def reset():
                    transport.resets += 1
                    if failure == "reset":
                        raise libusb.LibusbError("reset", -1)
                    if failure == "disappear":
                        (root / "1-2.1" / "idVendor").unlink()
                    if failure == "endpoints":
                        (root / "1-2.1:1.0/ep_01/wMaxPacketSize").write_text("0000")

                transport.reset = reset
                # When
                report = target.run(approved_options(cycles=2), make_env(root, lambda *a: transport))
                # Then
                self.assertFalse(report.qualified)
                self.assertFalse(report.cycles[0].reestablished_aoa)
                self.assertEqual(len(report.cycles), 1)
                self.assertTrue(transport.closed)

    def test_zero_duration_presence_requires_positive_aoa_sample(self):
        for ids in (None, APPROVED, AOAP):
            with self.subTest(ids=ids), TemporaryDirectory() as tmp:
                # Given
                root = Path(tmp) / "usb"
                if ids is not None:
                    write_port(root, "1-2.1", *ids)
                # When
                presence = target.observe(approved_options(), make_env(root, lambda *a: None))
                # Then
                self.assertEqual(presence.samples, 1)
                self.assertEqual(presence.stable, ids == AOAP)

    def test_unstable_presence_prevents_qualification(self):
        with TemporaryDirectory() as tmp:
            # Given: the device disappears after the first AOA sample.
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", *AOAP)
            clock = FakeClock()
            env = make_env(root, lambda *a: None, clock)

            def disappear(seconds):
                clock.sleep(seconds)
                (root / "1-2.1" / "idVendor").unlink(missing_ok=True)

            env = target.ProbeEnvironment(env.sysfs, env.transport_factory, env.runner,
                                          env.udc_root, env.configfs_root, clock, disappear)
            # When
            report = target.run(approved_options(duration=2), env)
            # Then
            self.assertFalse(report.qualified)
            self.assertIn("usb-presence-unstable", report.blockers)

    def test_final_device_and_gadget_are_recorded_after_presence(self):
        with TemporaryDirectory() as tmp:
            # Given: endpoints become unusable and gadget state changes during observation.
            root = Path(tmp) / "usb"
            write_port(root, "1-2.1", *AOAP)
            env = make_env(root, lambda *a: None)
            udc = env.udc_root / "test-udc"
            udc.mkdir(parents=True)
            (udc / "state").write_text("default")
            gadget = env.configfs_root / "test-gadget"
            gadget.mkdir(parents=True)
            (gadget / "UDC").write_text("test-udc")

            def change_state(seconds):
                env.sleep(seconds)
                (udc / "state").write_text("configured")
                (root / "1-2.1:1.0/ep_01/wMaxPacketSize").write_text("0000")

            changed_env = target.ProbeEnvironment(env.sysfs, env.transport_factory, env.runner,
                                                  env.udc_root, env.configfs_root, env.clock, change_state)
            # When
            report = target.run(approved_options(duration=1), changed_env)
            # Then
            self.assertFalse(report.qualified)
            self.assertEqual(report.gadget_before.state, "default")
            self.assertEqual(report.gadget.state, "configured")
            self.assertTrue(any(ep.max_packet_size == 0 for ep in report.device.endpoints))


class Cli(unittest.TestCase):
    def test_real_cli_requires_port_and_phone(self):
        result = subprocess.run(["bash", str(ROOT / "tools/target/probe-usb-aoa.sh"),
                                 "--jetson", "jetson.local"], capture_output=True, text=True, check=False)
        report = json.loads(result.stdout)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(report["blockers"], ["port-and-phone-id-required"])
        self.assertEqual(report["schema"], "aa-usb-aoa-probe/1")
        self.assertEqual(report["protected_inventory"]["status"], "external-proof-pending")

    def test_help_is_direct(self):
        result = subprocess.run(["bash", str(ROOT / "tools/target/probe-usb-aoa.sh"), "--help"],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0)
        self.assertIn("--port", result.stdout)
        target_help = subprocess.run(["python3", "-B", str(ROOT / "tools/target/usb_aoa_target.py"),
                                      "--help"], capture_output=True, text=True, check=False)
        self.assertEqual(target_help.returncode, 0)
        self.assertIn("--sysfs-root", target_help.stdout)

    def test_gate_never_opens_ssh_without_selection(self):
        with patch.object(sys, "argv", ["probe", "--jetson", "jetson.local"]), \
                patch.object(host.subprocess, "run") as run, redirect_stdout(io.StringIO()):
            self.assertEqual(host.main(), 1)
        run.assert_not_called()

    def test_target_invalid_arguments_are_json(self):
        output = io.StringIO()
        with redirect_stdout(output):
            code = target.main(["--port", "bad/port", "--phone-id", "zz"])
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(output.getvalue())["blockers"], ["invalid-arguments"])

    def test_target_cli_reports_qualification_only_with_usable_aoa(self):
        for usable in (True, False):
            with self.subTest(usable=usable), TemporaryDirectory() as tmp:
                # Given
                root = Path(tmp) / "usb"
                write_port(root, "1-2.1", *AOAP)
                if not usable:
                    (root / "1-2.1:1.0/ep_01/wMaxPacketSize").write_text("0000")
                output = io.StringIO()
                # When
                with patch.object(target, "SubprocessRunner", return_value=FakeRunner()), \
                        patch.object(target._LazyLibusb, "open") as open_usb, redirect_stdout(output):
                    code = target.main(["--port", "1-2.1", "--phone-id", "04e8:6860", "--duration", "0",
                                        "--sysfs-root", str(root), "--udc-root", str(root / "udc"),
                                        "--configfs-root", str(root / "configfs")])
                # Then
                report = json.loads(output.getvalue())
                self.assertEqual(code, 0 if usable else 1)
                self.assertEqual(report["qualified"], usable)
                self.assertEqual(report["gadget_before"]["configured"], False)
                open_usb.assert_not_called()

    def test_ssh_prefix_uses_fixed_safe_options(self):
        args = host._parser().parse_args(["--jetson", "jetson.local", "--source-address", "192.168.55.2",
                                          "--address", "192.168.55.3", "--host-key-alias", "jetson.local"])
        prefix = host._ssh_prefix(args)
        self.assertEqual(prefix[0], "ssh")
        self.assertIn("StrictHostKeyChecking=yes", prefix)
        self.assertIn("HostKeyAlgorithms=ssh-ed25519", prefix)
        self.assertIn("ControlMaster=no", prefix)
        self.assertIn("ControlPath=none", prefix)
        self.assertIn("HostKeyAlias=jetson.local", prefix)
        self.assertEqual(prefix[-1], "192.168.55.3")
        self.assertEqual(prefix[prefix.index("-b") + 1], "192.168.55.2")

    def test_payload_streams_all_target_modules(self):
        payload = host._payload(ROOT / "tools/target", ["--port", "1-2.1"])
        for name in ("usb_aoa_libusb", "usb_aoa_sysfs", "usb_aoa_hostenv", "usb_aoa_target"):
            self.assertIn(name, payload)
        self.assertIn("sys.exit(module.main", payload)

    def _target_payload(self):
        return {"schema": "aa-usb-aoa-probe/1", "target": "jetson.local", "qualified": True,
                "blockers": [], "protected_inventory": {}}

    def test_success_merges_target_report(self):
        calls = []

        def run(argv, **kwargs):
            calls.append(argv)
            if argv[-1] == "true":
                return SimpleNamespace(returncode=0, stdout="", stderr="")
            return SimpleNamespace(returncode=0, stdout=json.dumps(self._target_payload()), stderr="diag")

        output = io.StringIO()
        argv = ["probe", "--jetson", "jetson.local", "--port", "1-2.1", "--phone-id", "04e8:6860",
                "--duration", "0"]
        with patch.object(sys, "argv", argv), patch.object(host.subprocess, "run", side_effect=run), \
                redirect_stdout(output):
            code = host.main()
        self.assertEqual(code, 0)
        merged = json.loads(output.getvalue())
        self.assertEqual(merged["qualified"], True)
        self.assertEqual(merged["protected_inventory"]["status"], "external-proof-pending")
        self.assertEqual(calls[1][-1], "-")

    def test_preflight_failure_stops_before_streaming(self):
        calls = []

        def run(argv, **kwargs):
            calls.append(argv)
            return SimpleNamespace(returncode=1, stdout="", stderr="no route")

        output = io.StringIO()
        argv = ["probe", "--jetson", "jetson.local", "--port", "1-2.1", "--phone-id", "04e8:6860"]
        with patch.object(sys, "argv", argv), patch.object(host.subprocess, "run", side_effect=run), \
                redirect_stdout(output):
            code = host.main()
        self.assertEqual(code, 1)
        self.assertEqual(len(calls), 1)
        self.assertEqual(json.loads(output.getvalue())["blockers"], ["ssh-preflight-failed"])

    def test_inventory_mismatch_is_blocker(self):
        def run(argv, **kwargs):
            if argv[-1] == "true":
                return SimpleNamespace(returncode=0, stdout="", stderr="")
            return SimpleNamespace(returncode=0, stdout=json.dumps(self._target_payload()), stderr="")

        argv = ["probe", "--jetson", "jetson.local", "--port", "1-2.1", "--phone-id", "04e8:6860",
                "--duration", "0", "--before-manifest", "/b.json", "--after-manifest", "/a.json"]
        output = io.StringIO()
        with patch.object(sys, "argv", argv), patch.object(host.subprocess, "run", side_effect=run), \
                patch.object(host, "diff_manifests",
                             return_value=SimpleNamespace(verdict="different", exit_status=1)), \
                redirect_stdout(output):
            code = host.main()
        report = json.loads(output.getvalue())
        self.assertEqual(code, 1)
        self.assertIn("protected-inventory-not-equivalent", report["blockers"])
        self.assertEqual(report["protected_inventory"]["status"], "validated")


if __name__ == "__main__":
    groups = {"selection": Selection, "handshake": Handshake, "libusb": Libusb,
              "sysfs": SysfsCase, "hostenv": HostenvCase, "cycles": Cycles, "cli": Cli}
    selected = [groups[sys.argv[1]]] if len(sys.argv) > 1 else groups.values()
    suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(group) for group in selected)
    sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
