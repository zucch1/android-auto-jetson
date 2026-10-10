# S24 credential probe plan — three-arm differential (`s24-credential-probe-plan.md`)

**Status: PLAN ONLY — requires the owner's phone. Do not attempt without the
owner. No probe has been run; this document defines the protocol and the
decision table for the staged dual-track Stage 2.**
Prepared 2026-10-10 alongside `license-remediation-2026-10-10.json`.

## Purpose

Stage 1 (the license remediation) removed the published head-unit credential
and made `HU_KEY_PATH` the only identity source (fail-closed). Stage 2 must
decide the permanent credential strategy. The unknown is **what the phone
actually validates** when accepting the receiver/head-unit credential. This
probe distinguishes three phone-side behaviors with a controlled differential:

| Arm | Credential | Identity |
|---|---|---|
| (a) control | **real credential, operator-supplied** (the owner's known-good head-unit identity; not the historical published key) | real chain/identity |
| (b) neutral self-signed | generated synthetic self-signed credential, neutral DN (`CN=android-auto-jetson-synthetic-hu`, `OU=SYNTHETIC-TEST-FIXTURE`, `O=android-auto-jetson`) | unknown identity |
| (c) DN-mimic chain | operator-generated throwaway local CA + leaf, both keys fresh, chain anchored nowhere: leaf **subject** DN = historical subject DN (`C=JP, ST=Tokyo, L=Hachioji, O=JVC Kenwood, OU=01`), CA **subject** DN = historical **issuer** DN (`C=US, ST=California, L=Mountain View, O=Google Automotive Link`) so the leaf's issuer name matches the historical issuer | right names (subject and issuer independently controlled), wrong chain |

A self-signed mimic cannot play arm (c): `openssl req -x509` forces
subject == issuer, while the historical certificate has distinct subject and
issuer, so a self-signed mimic would fail any issuer-name check for the wrong
reason and could not separate issuer-name checking from chain checking. The
CA + leaf construction keeps both names right (leaf subject = historical
subject; leaf issuer = CA subject = historical issuer) while the chain itself
stays untrusted, which is exactly the remaining discrimination between
"names checked, trust not checked" and "trust chain checked".

Arms (b) and (c) are generated **only at probe time** into operator-controlled
paths; no DN-mimic material is ever committed (the tree scanner rejects
credential material and third-party DN names in material would recreate the
exposure Stage 1 removed). Arm (c) is a diagnostic instrument for the probe
window only; if it succeeds, its legal use is itself a Stage 2 question for
counsel. The throwaway CA key and certificate stay outside the probe bundle
and are destroyed after the probe window.

## Preconditions

- Jetson receiver built from `aa/license-remediation` (or later); host suite
  green; `HU_KEY_PATH` loader verified (`tls_posture` suite).
- Owner's phone (the approved phone from the task-8 predicate), USB cable,
  `adb` access to the phone for logcat capture.
- Receiver runs with the approved-phone predicate armed (unknown phones stay
  fail-closed; no permissive switches).

## Procedure (repeat once per arm)

1. **Prepare the bundle.** Build an owner-only PEM bundle (certificate then
   unencrypted key, mode `0600`, single hard link, no symlink) at an
   operator-controlled path, e.g. `$XDG_RUNTIME_DIR/aa-probe-armN.pem`:
   - (a) operator-supplied real credential (operator material),
   - (b) `python3 -B tools/tls/synthetic_credential.py <probe-dir>`
     output (`synthetic-hu.crt` + `synthetic-hu.key`),
   - (c) an operator-generated, fresh-key, untrusted-issued mimic with
     **independently controlled subject and issuer**: a throwaway local CA
     whose subject DN equals the historical issuer DN, and a leaf issued by
     that CA whose subject DN equals the historical subject DN, e.g.:

     ```
     openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 2 \
       -subj "/C=US/ST=California/L=Mountain View/O=Google Automotive Link" \
       -keyout ca.key -out ca.crt
     openssl req -newkey rsa:2048 -nodes -sha256 \
       -subj "/C=JP/ST=Tokyo/L=Hachioji/O=JVC Kenwood/OU=01" \
       -keyout leaf.key -out leaf.csr
     openssl x509 -req -sha256 -days 2 -in leaf.csr \
       -CA ca.crt -CAkey ca.key -CAcreateserial -out leaf.crt
     ```

     then bundle `leaf.crt` + `leaf.key` only. Do not use
     `openssl req -x509 -subj` for this arm: that forces subject == issuer
     and cannot represent the historical subject/issuer split.
2. **Wire HU_KEY_PATH.** `export HU_KEY_PATH=<bundle>` in the receiver
   service environment (systemd `Environment=HU_KEY_PATH=…` or the launch
   wrapper). Confirm the loader accepts it: the startup telemetry must show
   `tls-mode=encryption-only-compatibility` and no `tls-credential-*`
   failure class. Never log the path or PEM contents.
3. **Capture points — receiver side.** Start the receiver with structured
   telemetry on. Record: `tls-mode=` line, `tls-phone-not-approved` /
   `tls-peer-verification-rejected` / `tls-session-inactive` classes,
   session-state machine transitions (idle → connecting → connected), and
   the first encrypted-frame event.
4. **Capture points — phone side (ADB logcat).** Before connecting:
   `adb logcat -c`. Connect the phone over USB and trigger projection. After
   the attempt window (≤60 s), dump:
   `adb logcat -v threadtime -d > s24-arm<N>-logcat.txt`.
   Filter the dump for the Android Auto projection stack and TLS/cert
   keywords:
   `grep -inE 'gearhead|projection|android.?auto|tls|ssl|handshake|certificate|trust|verify' s24-arm<N>-logcat.txt`.
   Record whether the phone reached the encrypted projection session
   (video/audio flowing to the dummy display) or aborted, and which
   validation message (if any) the phone logged.
5. **Verdict per arm.** `PASS` = phone completes the Android Auto session
   with encrypted payload over the wire (receiver telemetry:
   `encrypted-payload=yes` equivalent in the live session, session state
   connected) within the attempt window. `FAIL` = session does not establish
   (phone-side abort or TLS failure), regardless of receiver state.
6. **Isolation.** Run arms in order (a), (b), (c) with the phone unpaired /
   session torn down between arms; capture fresh logs per arm. Do not reuse
   bundles between arms.

## Outcome interpretation

Read every row as a **hypothesis** supported by this phone/build and these
credentials only. No row proves "any credential" acceptance in general, chain
validation in general, or the existence of a DN blocklist; those claims need
more devices, builds and credentials than this probe provides. An unexplained
or non-TLS failure (receiver crash, cable/predicate abort, logcat with no
TLS or validation signal) is **inconclusive**: do not score it into any row
below, and do not treat it as evidence for or against a hypothesis.

| (a) real | (b) neutral self-signed | (c) DN-mimic chain | Branch | Hypothesis (this phone/build, these credentials) | Stage 2 consequence |
|---|---|---|---|---|---|
| PASS | PASS | PASS | **permissive** | Consistent with accepting any well-formed receiver credential in this window; no name or chain signal observed | **Self-signed generator is sufficient.** Ship the synthetic/neutral generator as the documented path; operator-supplied stays optional. Dual-track converges on the generator track. |
| PASS | FAIL | PASS | **name-check** | Consistent with checking DN names (the (c) leaf's subject and/or issuer name matters) but not the trust chain: (c) carries both historical names on an untrusted chain and still passed | Self-signed neutral identity will not work in the field. Options: (i) operator-supplied permanent credential, or (ii) a DN-mimic generator — but mimicking the third-party DN is exactly the exposure Stage 1 removed; (ii) requires explicit counsel sign-off. Default: **operator-supplied permanent**. |
| PASS | FAIL | FAIL | **chain-or-pin** | Consistent with requiring a trusted chain or the exact operator credential. NOT proof of chain validation specifically: credential pinning produces the same table | Only real chained credentials work. **Operator-supplied permanent is required**; no generator track. HU_KEY_PATH remains the sole identity source. |
| FAIL | * | * | probe invalid | Control arm must pass or the setup is wrong (cable, predicate, receiver build) | Fix the setup and rerun; no Stage 2 decision. |
| PASS | PASS | FAIL | unexpected (name-avoidance) | Consistent with the phone accepting unknown identities yet rejecting the historical names (e.g. an explicit DN denylist); one probe cannot prove a blocklist | Treat as permissive for the generator track but record the observation; counsel question only if the owner reopens it (see disclosure note below). |
| any | any | unexplained/non-TLS | **inconclusive** | No hypothesis supported | Analyze the failure class first; rerun the affected arm(s); no Stage 2 decision. |

## Stage 2 decision rule (staged dual-track)

- **permissive** → Stage 2 lands the self-signed generator track (documented,
  generated per deployment, neutral DN; no secret shipped). Operator-supplied
  remains supported by `HU_KEY_PATH`.
- **name-check / chain-or-pin** → Stage 2 lands operator-supplied
  permanent as the supported path (fail-closed loader unchanged); the
  generator stays test-only.
- Disclosure note (recorded owner decision, 2026-10-10): responsible
  disclosure of the historical credential was **declined** — "not our
  problem; potential trade-off too high". The draft
  `license-remediation-2026-10-10-disclosure-draft.md` remains an unsent
  draft and must not be sent. A name-check or chain-or-pin outcome does not
  change that decision; reopening disclosure would require a **new owner
  decision** recorded as such, not an automatic escalation.

## Evidence to file after the probe

One receipt per arm (logcat dump path + receiver telemetry excerpt + verdict)
plus a probe summary updating this table with observed outcomes, the phone
model/build, receiver commit, and the Stage 2 decision taken. Owner presence
is required; nothing in this plan has been executed.
