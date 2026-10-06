# Wireless capability probe (task 12)

This phase is host implementation only. No Jetson or phone has been contacted.
Run host fixtures with `python3 -B tests/wireless/test_probe.py`,
`python3 -B tests/wireless/test_regulatory.py`, and
`python3 -B tests/wireless/test_psk.py`. CTest registers six original scenario groups
and ten individually named correction regressions; CI requires all sixteen names.
Reports use `aa-wireless-probe/1`.
Discovery does not imply AP operation or BlueZ registration. Failed capabilities
are blockers, never pairing, trust, projection, phone-hosted AP, or P2P claims.

## Separate owner-authorized target window

The default `tools/target/probe-wireless.sh --jetson jetson.local` emits a JSON
window-required blocker without contact. In an authorized window, provide
`--window-hook /absolute/owner-approved-hook --window-id ID
--codegraph-root /exact/approved/referent`. The hook is the trusted authorization
boundary; merely choosing an ID is not authorization. It must reject stale,
unapproved, or nonserialized windows. The probe never grants a target window.

The executable hook receives `WINDOW_ID TARGET PHASE`. Each invocation has a
900-second ceiling. Hook stdout for `before` and `after` is exactly one absolute
HOST path to a complete retained `ppic/1` manifest; other phases emit no data.
Hook operations must use the existing `tools.target_safety.snapshot_cli` producer,
record every target scratch path in a host-retained trace, include exactly the
protected infotainment root and approved codegraph referent (in that order),
record the supplied target/window identity, and retain full manifests under
`.omo/evidence/jetson-android-auto-receiver/`. Nonzero hook exits block the probe.

Ordering follows `execution-task9-inventory-window-ruling.json` and its corrigendum:

1. `before`: set up only the designated authorized task-12 scratch directory and
   inventory tooling there; first protected traversal precedes all other operations.
2. SSH BatchMode/strict-known-host preflight, then read-only discovery.
3. Optional guarded active workload.
4. `workload-cleanup`: remove workload artifacts/effects, leaving inventory area.
5. `after`: traverse final protected scope and retrieve full host manifest. The
   existing canonical diff consumer verifies equivalence and retains a diff artifact.
6. `final-cleanup`: remove all remaining authorized scratch and perform the required
   read-only existence check. This hook runs even on failures; hooks must be idempotent.

Closing operations are attempted on probe/preflight failures. If contact is lost,
cleanup is a blocker, not an assertion of restoration. Hooks own target scratch
cleanup, authorization expiry, and recovery; the probe does not overwrite protected
paths or install packages. Sources stream over SSH stdin with no target source files.

## Active probe requirements

Add `--active --jurisdiction-confirm=US --ap-psk-file /host/private/psk` and optionally
`--interface wlan0 --duration 30` (1..120 seconds). An explicit ISO country override
is supported; the effective phy regulatory country must match it. The script never
sets the regulatory domain. Missing confirmation still permits read-only discovery,
but never AP startup. Channel 36 must be enabled for AP initiation in the selected
phy and effective regulatory rules; unknown, disabled, no-IR, radar/DFS, or insufficient
20 MHz range is conservatively rejected. A self-managed phy supersedes global rules.
All regulatory sections must parse completely before selection: duplicate sections
or countries, malformed or overlapping rules, and ambiguous numeric phy aliases
block startup rather than falling back to global permission. ASCII case and ordinary
whitespace are normalized. Unknown rule flags cannot grant permission, and only a
complete unflagged channel power annotation is accepted as channel evidence.

The window requires RTL8822CE identity, `iw`, NetworkManager/nmcli, busctl, BlueZ,
existing target Python dbus/GLib bindings, and already-authorized NM/D-Bus permissions
(no sudo prompts or installations). The interface must be disconnected and isolated,
not carrying management connectivity. NetworkManager creates an in-memory profile
(`save no`, autoconnect off), channel 36/5 GHz, WPA-PSK, with IPv4/IPv6 disabled;
no DHCP, NAT, forwarding, persistent profile, or regulatory changes are made.
The UUID-specific profile is deleted in a finally block. Before any window-hook or
SSH invocation, the host opens the PSK with `O_NOFOLLOW` and validates the opened
inode with `fstat`: a regular file owned by the current effective user, one hard link,
and no group/other permissions. Symlink inputs, FIFOs, missing files, invalid owner
or permissions, and invalid content are rejected without contact. Reads are bounded
and use that same descriptor, not a later reopening of the path. Supported input is
8..63 printable ASCII characters, no leading/trailing spaces, with one optional final
LF. PSK travels via SSH stdin and then NetworkManager's `connection up passwd-file
/dev/stdin` input; it never enters subprocess argv or JSON. The in-memory profile
sets PSK flags to `not-saved` (2); diagnostics record error types, not credential-bearing
exception messages. It is an ephemeral fixture credential, not a production key.

An owner-approved **non-phone test client** must associate during the observation
interval. The probe records station association, not IP connectivity, pairing, or
projection. It verifies actual AP type and frequency rather than echoing configuration.
BlueZ exports a custom Profile1 and rejecting Agent1, registers both, services the
event context, then unregisters/disconnects. It never requests default-agent status,
calls Pair, changes Trusted, or initiates Bluetooth connections. Callback names are
observations only; behavioral pairing assertions remain deferred to task 36.

Target qualification is always false in this probe report. Actual target capability
results and complete protected-path evidence are pending the separate gated window.
