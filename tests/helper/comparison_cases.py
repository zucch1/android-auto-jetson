# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/helper/bluez_runtime.py build/host-dev
from __future__ import annotations

from typing import TYPE_CHECKING, TypedDict
from gi.repository import GLib

if TYPE_CHECKING:
    from bluez_runtime import Surface


class Challenge(TypedDict):
    challenge_id: str
    comparison: str
    address: str
    adapter: str


def present(surface: Surface, request: int) -> Challenge:
    from bluez_runtime import control, wait
    challenge: Challenge = {"challenge_id": "", "comparison": "", "address": "", "adapter": ""}
    def fetched() -> bool:
        try:
            result = control(surface.consumer, "GetPairingComparison", GLib.Variant("(t)", (request,))).unpack()[0]
            challenge.update(result)
            return True
        except GLib.Error:
            return False
    wait(fetched)
    return challenge


def affirmative(surface: Surface, request: int) -> None:
    from bluez_runtime import command, control, rejected, wait
    command(surface.mock, "comparison-start")
    challenge = present(surface, request)
    assert challenge["comparison"] == "000042"
    assert challenge["address"] == "02:00:00:00:00:01"
    assert command(surface.mock, "comparison-result") == "pending"
    assert command(surface.mock, "RequestConfirmation") == "rejected"
    rejected(surface.rogue, "GetPairingComparison", GLib.Variant("(t)", (request,)))
    response = GLib.Variant("(ttb)", (request, int(challenge["challenge_id"]), True))
    rejected(surface.rogue, "RespondPairingComparison", response)
    rejected(surface.consumer, "RespondPairingComparison", GLib.Variant("(ttb)", (request + 1, int(challenge["challenge_id"]), True)))
    rejected(surface.consumer, "RespondPairingComparison", GLib.Variant("(ttb)", (request, int(challenge["challenge_id"]) ^ 1, True)))
    control(surface.consumer, "RespondPairingComparison", response)
    wait(lambda: command(surface.mock, "comparison-result") == "allowed")
    rejected(surface.consumer, "RespondPairingComparison", response)
    assert command(surface.mock, "RequestConfirmation") == "rejected"


def negative_comparisons(surface: Surface) -> None:
    from bluez_runtime import command, control, rejected, wait
    for action in ("reject", "cancel", "Cancel", "recreate", "adapter-recreate", "observe-other", "release-name", "consumer-loss"):
        command(surface.mock, "observe-first")
        request = surface.request()
        surface.confirm(request)
        for unsupported in ("RequestPinCode", "RequestPasskey"):
            assert command(surface.mock, unsupported) == "rejected"
        assert command(surface.mock, "RequestConfirmation wrong") == "rejected"
        assert command(surface.mock, "RequestConfirmation rogue") == "rejected"
        assert command(surface.mock, "RequestConfirmation overflow") == "rejected"
        command(surface.mock, "comparison-start")
        challenge = present(surface, request)
        response = GLib.Variant("(ttb)", (request, int(challenge["challenge_id"]), True))
        match action:
            case "reject":
                control(surface.consumer, "RespondPairingComparison", GLib.Variant("(ttb)", (request, int(challenge["challenge_id"]), False)))
            case "cancel":
                surface.cancel(request)
            case "consumer-loss":
                surface.consumer.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                    "ReleaseName", GLib.Variant("(s)", ("org.custom.BluezConsumer",)), None, 0, 1000, None)
            case _:
                command(surface.mock, action)
        rejected(surface.consumer, "RespondPairingComparison", response)
        wait(lambda: command(surface.mock, "comparison-result") == "rejected")
        if action == "release-name":
            command(surface.mock, "acquire-name")
            wait(lambda: bool(surface.state()["profile"]))
        if action == "consumer-loss":
            surface.consumer.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                "RequestName", GLib.Variant("(su)", ("org.custom.BluezConsumer", 0)), None, 0, 1000, None)
            control(surface.consumer, "RegisterConsumer", GLib.Variant("(s)", ("org.custom.BluezConsumer",)))
        if action not in ("cancel", "consumer-loss"):
            surface.cancel(request)
        rejected(surface.consumer, "RespondPairingComparison", response)
    print("PASS comparison_reject_cancel_peer_incarnation_owner_and_consumer_loss", flush=True)


def probe_flow(surface: Surface) -> None:
    import time
    from bluez_runtime import command, wait
    def rpc(text: str) -> tuple[int, int]:
        values = command(surface.receiver, text).split()
        return int(values[0]), int(values[1])
    assert rpc("targetless")[0] == 1001
    assert rpc("malformed")[0] == 1002
    error, handle = rpc("poll")
    assert error == 0 and handle
    assert rpc(f"bind {handle} 1")[0] == 0
    error, grant = rpc("issue 1")
    assert error == 0 and rpc(f"open {grant}")[0] == 0
    assert rpc(f"open {grant}")[0] == 1001
    assert rpc("close")[0] == 0
    assert rpc(f"bind {handle} 2")[0] == 0
    error, grant = rpc("issue 2")
    assert error == 0 and rpc(f"open {grant}")[0] == 1001
    assert rpc("close")[0] == 0
    error, stale = rpc("poll")
    assert error == 0
    command(surface.mock, "release-name")
    wait(lambda: command(surface.mock, "RequestAuthorization") == "rejected")
    command(surface.mock, "acquire-name")
    wait(lambda: bool(surface.state()["profile"]))
    assert rpc(f"reconnect {stale}")[0] == 1001
    error, expiring = rpc("poll")
    assert error == 0
    error, fresh = rpc("poll")
    assert error == 0 and rpc(f"bind {fresh} 3")[0] == 0
    error, grant = rpc("issue 3")
    assert error == 0 and rpc(f"open {grant}")[0] == 0
    command(surface.mock, "comparison-start")
    challenge = 0
    def fetched() -> bool:
        nonlocal challenge
        error, challenge = rpc("comparison 3")
        return error == 0 and challenge != 0
    wait(fetched)
    assert rpc("comparison 4")[0] == 1001
    assert rpc(f"respond 4 {challenge}")[0] == 1001
    assert command(surface.mock, "comparison-result") == "pending"
    command(surface.mock, "paired")
    assert command(surface.mock, "NewConnection") == "allowed"
    error, expired_grant = rpc("issue 4")
    assert error == 0
    started = time.monotonic()
    wait(lambda: not surface.state()["properties"]["Pairable"], 62)
    assert 59 <= time.monotonic() - started <= 62
    assert rpc("comparison 3")[0] == 1001
    assert rpc(f"respond 3 {challenge}")[0] == 1001
    assert command(surface.mock, "comparison-result") == "rejected"
    assert rpc(f"reconnect {expiring}")[0] == 1001
    assert rpc(f"open {expired_grant}")[0] == 1001
    assert command(surface.mock, "fd-eof") == "closed"
    assert command(surface.mock, "RequestAuthorization") == "rejected"
    print("PASS comparison_timeout_wrong_source_and_monotonic60s_idle_expiry", flush=True)
