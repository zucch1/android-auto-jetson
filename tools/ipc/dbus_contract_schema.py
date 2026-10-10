#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Checked schema rules for the D-Bus control contract (task 21).

The introspection XML is the contract authority; these tables are the
machine-readable checked schema it must match exactly: interface identity,
the twelve method signatures, five events, the SemVer metadata and the
argument-type allowlist that keeps audio/video and every other high-rate
payload off the control surface.
"""
from __future__ import annotations

import re
from typing import Final

INTERFACE: Final[str] = "org.custom.AndroidAutoReceiver1"
OBJECT_PATH: Final[str] = "/org/custom/AndroidAutoReceiver"
SEMVER: Final[str] = "1.0.0"
SEMVER_ANNOTATION: Final[str] = "org.custom.AndroidAutoReceiver1.ContractSemVer"
INTERFACE_PREFIX: Final[str] = "org.custom.AndroidAutoReceiver"

# name -> (in signature, out signature)
METHODS: Final[dict[str, tuple[str, str]]] = {
    "RegisterConsumer": ("s", "t"),
    "UnregisterConsumer": ("", ""),
    "StartProjection": ("", ""),
    "StopProjection": ("", ""),
    "RequestAddPhone": ("", "t"),
    "ConfirmPhonePairing": ("t", ""),
    "CancelPhonePairing": ("t", ""),
    "ForgetPhone": ("t", ""),
    "GetState": ("", "st"),
    "GetCapabilities": ("", "a{ss}"),
    "SetDisplayViewport": ("uuu", ""),
    "Ping": ("", "t"),
}
# name -> full signal argument signature
SIGNALS: Final[dict[str, str]] = {
    "PairingRequest": "t",
    "PairingOpened": "t",
    "PairingClosed": "t",
    "StateChanged": "st",
    "DiagnosticEvent": "sst",
}
PROPERTIES: Final[dict[str, str]] = {"ContractSemVer": "s"}
ALLOWED_SCALAR_TYPES: Final[frozenset[str]] = frozenset({"s", "b", "u", "t"})
ALLOWED_MAP_TYPE: Final[str] = "a{ss}"
MAJOR: Final[re.Pattern[str]] = re.compile(
    rf"^{re.escape(INTERFACE_PREFIX)}(0|[1-9][0-9]*)$")


