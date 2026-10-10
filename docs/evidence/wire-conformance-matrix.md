# Wire-conformance matrix (schema `aa-wire-conformance-1`)

Pre-28 conformance audit: project-owned protocol behavior vs the OAA wire rulebook.
Machine-readable source of truth: `wire-conformance-matrix.json` (same directory).

- **Rulebook**: mrmees/open-android-auto @ `61eab61c5f9968154ff1a80faa8c0a427b208479`
  (local clone `/tmp/opencode/oaa-rulebook`; in-repo reference copy
  `third_party/reference/open-android-auto/61eab61c.../`, byte-identical for all 248
  `oaa/**` protos per `PROVENANCE.md:8`). Reference only — never in the build.
- **Audited tree**: `aa/ci-arm64-cross-gate` @ `57f4045`, worktree `/tmp/opencode/conformance-prep`,
  audit branch `aa/pre28-conformance-audit`. Ours = repo `path:line`; AASDK = `third_party/aasdk/...`.
- **Citation ranking when OAA self-contradicts**: 17.3 endpoint message-matrix
  (`analysis/reports/android-auto-17.3-update/message-matrix.md`, confirmed-static) >
  gold/silver proto comments (APK + live capture) > `docs/cross-version/*` corrections >
  channel docs > interaction docs. Flagged OAA-internal defects: D18, D19, D20.
- **Legend** — status: `implemented` (codec consume/emit) / `partial` (enum only) / `absent`.
  Confidence: gold / silver / bronze / unverified / retracted (rulebook sidecars).
  Directions: phone→HU / HU→phone (from the HU's perspective).

**Totals**: 35 rows (R01-R18, R20-R36; R19 unassigned), 66 field rows, 24 discrepancies (2 anchors), 21 raw-byte fixture forms.

---

## 1. Control channel (frame channel 0, `MessageKind::control`, 2-byte BE message id)

| Row | Message | ID | Dir | Required/default | Version cond. | OAA conf. | OAA evidence | Ours | Discrepancy |
|-----|---------|-----|-----|------------------|---------------|-----------|--------------|------|-------------|
| R01 | VersionRequest | 0x0001 | **HU→phone** (HU sends first) | body = major BE16 + minor BE16 raw (exactly 4 B); request must describe implemented behavior | before TLS, plaintext; gates v1.4/1.6/1.7 | unverified/doc | `oaa/control/ControlMessageIdsEnum.proto:15`; `docs/interactions/02-version-ssl-auth.md:55-94` | implemented (`Messages.hpp:128-130,174-181`; `ControlAdapter.cpp:63-89`) | D15, D16 |
| R02 | VersionResponse | 0x0002 | phone→HU | 8 B = id + major + minor + status (0x0000 MATCH / 0xFFFF MISMATCH); ≥v1.6 appends WireConfig tail | v1.6+ tail; AA 17.3 answers v1.7 or v6.1 | unverified/doc | `ControlMessageIdsEnum.proto:16`; `02-version-ssl-auth.md:103-148` | **absent** (handshake receive side) | D15 |
| R03 | ServiceDiscoveryRequest | 0x0005 | phone→HU (device info; OAA: sent *after* the HU response) | all optional; f1-3 PNG icons, f4 device_name, f5 device_brand, f6 SessionInfo{session_uuid} | all | silver (icons live-captured S25 Ultra 2026-02-24) | `oaa/control/ServiceDiscoveryRequestMessage.proto:10-25`; `docs/interactions/03:36-46,316-330` | **absent** (only bare 0x0005 in tests, `tests/replay/fixtures.hpp:103-105`) | D8, D16 |
| R04 | ServiceDiscoveryResponse | 0x0006 | HU→phone (OAA: sent **first**, proactive) | f1 repeated Service/ChannelDescriptor (id=1 required 1..255 + exactly one slot 2-14); f2-11 HU identity; f13 session_configuration bitmask; f14 display_name; f15 probe_for_support; f17 headunit_info; **no f12/f16** | slots 15-18 + marker bits 0x8000/0x10000/0x20000 = 17.x | silver; f16 retraction dated 2026-07 | `ServiceDiscoveryResponseMessage.proto:32-56`; `ChannelDescriptorData.proto:50-70`; `docs/interactions/03:48-80` | implemented (`ServiceAdapter.cpp:105-167`; `MediaConfiguration.cpp:24-152`) | D9, D16, D18 |
| R05 | ChannelOpenRequest | 0x0007 | phone→HU (proto: APK sender `hyr.java`) | f1 priority **sint32 zigzag** required; f2 service_id/channel_id int32 required (dynamic id) | all | silver | `ChannelOpenRequestMessage.proto:10-22` (conflicts with `04-channel-lifecycle.md:58-75`) | implemented (`ControlAdapter.cpp:156-187`; `Negotiator.cpp:216-226`) | D17 |
| R06 | ChannelOpenResponse | 0x0008 | HU→phone | f1 status required enum; we accept only 0 SUCCESS / −250 COMMAND_NOT_SUPPORTED, else malformed | −26/−27 (OAA-only) are car-control era | unverified enum / silver msg | `ChannelOpenResponseMessage.proto:21-25`; `oaa/common/StatusEnum.proto:13-49` | implemented (`ControlAdapter.cpp:189-219`) | D10, D17 |
| R07 | ChannelCloseNotification | 0x0009 | bidirectional | teardown announcement | all | silver family | `ControlMessageIdsEnum.proto:39`; `04:150-156` | partial (enum only, `Messages.hpp:137`) | — |
| R08 | PingRequest | 0x000B | bidirectional | f1 timestamp int64 required; f2 bug_report bool; f3 data bytes (≤64 KiB ours) | all | unverified | `ControlMessageIdsEnum.proto:23`; AASDK `PingRequest.proto:3-5` | implemented (`ControlAdapter.cpp:91-123`) | — |
| R09 | PingResponse | 0x000C | bidirectional | f1 timestamp required; f2 data optional | all | unverified | `ControlMessageIdsEnum.proto:24` | implemented (`ControlAdapter.cpp:125-154`) | D24 |
| R10 | NavigationFocusRequest | 0x000D | phone→HU | f1 type enum NATIVE=1 PROJECTED=2 (OAA optional) | all | silver | `NavigationFocusRequestMessage.proto:31-36` | partial (`Messages.hpp:140`) | D13 |
| R11 | NavigationFocusResponse (ours: nav_focus_notification) | 0x000E | HU→phone | f1 type; **OAA optional vs AASDK required** | all | silver | `NavigationFocusResponseMessage.proto:60-66` | partial (`Messages.hpp:141`) | D13 |
| R12 | AudioFocusRequest | 0x0012 | phone→HU | f1 focus_type enum 0-4 (3 was GAIN_NAVI) | GAL focus arbitration OFF | unverified | `ControlMessageIdsEnum.proto:30`; `AudioFocusTypeEnum.proto:24-28` | partial (`Messages.hpp:145`) | D21 |
| R13 | AudioFocusResponse (ours: audio_focus_notification) | 0x0013 | HU→phone | f1 focus_state enum 0-7 | all | unverified | `ControlMessageIdsEnum.proto:31` | partial (`Messages.hpp:146`) | D21 |
| R14 | VoiceSessionNotification (OAA: VOICE_SESSION_REQUEST) | 0x0011 | phone→HU (request semantics) | — | all | unverified | `ControlMessageIdsEnum.proto:29` | partial (`Messages.hpp:144`) | D22 |
| R15/R16 | ByeByeRequest/Response (OAA: SHUTDOWN_REQUEST/RESPONSE) | 0x000F/0x0010 | bidirectional teardown | — | all | unverified | `ControlMessageIdsEnum.proto:27-28` | partial (`Messages.hpp:142-143`) | D23 |
| R17 | ServiceDiscoveryUpdate | 0x001A | HU→phone | mid-session capability delta | all | unverified | `ControlMessageIdsEnum.proto:38` | partial (`Messages.hpp:153`) | — |
| R18 | unexpected_message / framing_error | 0x00FF/0xFFFF | **never on wire** | AASDK-internal sentinels; must never be encoded | n/a | absence in `ControlMessageIdsEnum.proto:14-39` | AASDK `ControlMessageType.proto:32-33` | enum values exist with an over-broad "wire contract" comment (`Messages.hpp:126-127,154-155`) | D14 |

### Field-level detail — handshake bytes (fixture-anchored)

| Field | Wire bytes | Type | Req | Evidence |
|-------|-----------|------|-----|----------|
| VersionRequest body | `major:2 BE, minor:2 BE` | raw | yes | `02-version-ssl-auth.md:72-94` |
| VersionResponse body | `major:2 BE, minor:2 BE, status:2 BE [+ WireConfig tail ≥v1.6]` | raw | yes | `02-version-ssl-auth.md:103-148` |
| ChannelOpenRequest.1 | varint **zigzag** | sint32 priority | yes | `ChannelOpenRequestMessage.proto:21` |
| ChannelOpenRequest.2 | varint | int32 service_id (dynamic) | yes | `ChannelOpenRequestMessage.proto:22` |
| ChannelOpenResponse.1 | varint sign-extended | enum status (0, 1, −1..−25, −250/−251/−253/−254/−255; OAA +−26/−27) | yes | `StatusEnum.proto:13-49` |
| PingRequest.1/2/3 | varint/varint/LEN | int64/bool/bytes | yes/no/no | AASDK `PingRequest.proto` |
| ServiceDiscoveryRequest.4/5 | LEN | string device_name / device_brand (**OAA live capture**) | no | `ServiceDiscoveryRequestMessage.proto:19-20` |
| ServiceDiscoveryResponse.1 | LEN repeated | Service{id:1 + one of slots 2-14} | yes(entry) | `Service.proto:21-37` |

---

## 2. AV / media service channels (dynamic frame byte; 2-byte BE message id; `MessageKind::specific`)

| Row | Message | ID | Dir | Required/default | Version cond. | OAA conf. | OAA evidence | Ours | Discrepancy |
|-----|---------|-----|-----|------------------|---------------|-----------|--------------|------|-------------|
| R20 | AVChannelSetupRequest (AASDK `Setup`) | 0x8000 | phone→HU | f1 media_codec_type required enum (PCM=1 AAC=2 H264=3 ADTS=4 [VP9=5 AV1=6 H265=7]) | modern codecs GAL 6.0 **OFF** | silver | `AVChannelSetupRequestMessage.proto:22-24`; `04:193-199` | **absent** (task 28) | — |
| R21 | AVChannelStartIndication | 0x8001 | phone→HU | f1 session int32 req; f2 config uint32 req; f3 session_type 0-2; f4 media_config (13-field) | f3 PDK≥5.0; f4 audio ≥5.0 / video ≥6.0 — **OFF** | silver (deep trace) | `AVChannelStartIndicationMessage.proto:23-28` | **absent** (task 28) | — |
| R22 | AVChannelStopIndication | 0x8002 | phone→HU | **empty body** | all | unverified/silver | `AVChannelStopIndicationMessage.proto:21-23`; `04:168-182` | **absent** (task 28) | — |
| R23 | AVChannelSetupResponse (AASDK `Config`, id named MEDIA_MESSAGE_CONFIG) | 0x8003 | HU→phone | f1 status req; f2 max_unacked uint32 (>0; 24 in ackless); f3 configs uint32 rep | ackless GAL 5.0 **OFF** | silver | `AVChannelSetupResponseMessage.proto:21-25`; `AVChannelSetupStatusEnum.proto:16-18` | **absent** (task 28) | **D4** |
| R24 | AVMediaAckIndication (AASDK `Ack`) | 0x8004 | HU→phone | f1 session_id int32 req; f2 ack uint32; f3 receive_timestamps uint64 rep | all | silver | `AVMediaAckIndicationMessage.proto:24-28` | **absent** (task 28) | — |
| R25 | VideoFocusRequest | 0x8007 | **phone→HU** (17.3: phone sends `xnd`) | f2 focus_mode enum 0-4; f3 focus_reason enum 0-4; **no f1** (AASDK keeps deprecated f1) | all | **gold** (17.3 endpoint) | `VideoFocusRequestMessage.proto:11-17`; `message-matrix.md:146-149` | **absent** (task 28) | **D5, D6**, D20 |
| R26 | VideoFocusIndication (AASDK `VideoFocusNotification`) | 0x8008 | **HU→phone** (17.3: phone parses `xnb`) | f1 focus_mode; f2 unrequested/unsolicited bool | all | gold | `VideoFocusIndicationMessage.proto:10-18`; `message-matrix.md:147-148` | **absent** (task 28) | D20 |
| R27 | UpdateUiConfigRequest (HU→phone dir) | 0x8009 | HU→phone (17.3: phone parses `xms`) | f1 AdditionalVideoConfig **required on wire** (missing → PROTOCOL_WRONG_MESSAGE / INVALID_UI_CONFIG) | f5-8 GAL 4.3+ **OFF** | gold (handler verified) | `UpdateUiConfigRequestMessage.proto:10-34`; `message-matrix.md:150` | **absent** (UI config) | **D1, D3** |
| R28 | UpdateUiConfigRequest (phone→HU dir) — **AASDK misnames this UPDATE_UI_CONFIG_REPLY** | 0x800A | phone→HU (17.3: phone sends `xms`) | same payload as 0x8009; **no reply implied** | as R27 | gold | `UpdateUiConfigRequestMessage.proto:14-26`; `UiConfigMessages.proto:12-19`; `message-matrix.md:151` | **absent** (anchor row) | **D1, D3** |
| R29 | UiConfigRequest (theming tokens) | 0x8011 | phone→HU (17.3: phone sends `xmt`) | f1 UiConfigData{entries key/value} | gated on color_scheme_support | silver+ | `UiConfigRequestMessage.proto:22-37`; `message-matrix.md:158-159` | **absent** (no AASDK id either) | D2 |
| R30 | **UpdateHuUiConfigResponse** (the true reply) | **0x8012** | HU→phone (17.3: phone parses `xmu`) | f1 status enum ERROR=0 ACCEPTED=1 REJECTED=2 | pairs with 0x8011 | status unverified / msg verified | `UpdateHuUiConfigResponse.proto:9-27`; `message-matrix.md:160-161` | **absent** — THE anchor | **D1, D2** |
| R31 | AudioUnderflowNotification | 0x800B | HU→phone | **no payload** on the wire (17.3 endpoint) | all | confirmed-static | `message-matrix.md:152-153` | **absent** | **D7** |
| R32 | (reserved) | 0x8010 | unknown | "reservation is not permission to shift a later name into that slot" | n/a | deferred | `AVChannelMessageIdsEnum.proto:31`; `cross-version/video.md:33` | absent (correctly) | — |
| R33 | AV media data | 0x0000 / 0x0001 | phone→HU | raw codec payload (+timestamps on 0x0000) | ackless GAL 5.0 OFF | silver | `AVChannelMessageIdsEnum.proto:14-15` | raw media via transport/replay records | — |
| R34 | MediaStats / MediaOptions | 0x8013 / 0x8014 | 0x8013 HU→phone; 0x8014 phone→HU | stats fields; options = 13-field envelope (semantics unresolved) | GAL 5.1/6.0 **OFF** | confirmed-static | `message-matrix.md:162-165` | **absent** | — |

### The anchor pair (discrepancy detail)

```
AASDK history (third_party/aasdk/.../MediaMessageId.proto:18-19):
  32777 0x8009  MEDIA_MESSAGE_UPDATE_UI_CONFIG_REQUEST
  32778 0x800A  MEDIA_MESSAGE_UPDATE_UI_CONFIG_REPLY     <-- name implies a reply
  (enum ends 32779 = 0x800B; no 0x8011, no 0x8012)

OAA wire truth (gold, 17.3 endpoint verified):
  0x8009  UpdateUiConfigRequest  HU -> phone   (AdditionalVideoConfig f1)
  0x800A  UpdateUiConfigRequest  phone -> HU   (same message, opposite direction)
          "No reply is implied." (oaa/av/UiConfigMessages.proto:18-19)
  0x8011  UiConfigRequest        phone -> HU   (theming tokens)
  0x8012  UpdateHuUiConfigResponse HU -> phone  (ThemingTokensStatus) <-- the reply
```

Task-28+ rule: implement 0x8009/0x800A as one bidirectional request message; answer the
theming request (0x8011) **only** via 0x8012. Never treat 0x800A as a reply.

---

## 3. Framing + identity spaces

| Row | Item | Truth | OAA evidence | Ours | Discrepancy |
|-----|------|-------|--------------|------|-------------|
| R35 | Frame header | `[channel:1][flags:1][size:2 BE \| 6 for FIRST][payload]`; flags: FT bits0-1 = 00 MIDDLE / 01 FIRST / 10 LAST / 11 BULK; bit2 M (0 SPECIFIC / 1 CONTROL); bit3 E; bits4-7 reserved, reject | `docs/channels/architecture.md:35-101` (cites aasdk FrameHeader/FrameType) | matches: `Frames.hpp:432-470`, `FrameCodec.cpp:63-133`, `MessageFramer.cpp:267-417`; reserved bits fail closed | D19 (OAA 02 doc says flags 0x00 for control-bulk — internal defect; 0x07 is correct) |
| R36 | ID spaces | wire frame byte = **dynamic** `ChannelDescriptor.channel_id` (f1, 1..255, 0=control); GAL service type = stable identity; SDK static ChannelId ordinal = reference-only | `channel-map.md:5-30` | matches design `Messages.hpp:56-57,112-118`; routing never uses ordinals (`Frames.hpp:440-442`) | **D12** (pinned-aasdk ordinals fork-drifted vs GAL types: 1 SENSOR-vs-INPUT, 7 TELEPHONY-vs-SENSOR, 8 INPUT-vs-AV_INPUT, 9 MIC-vs-BT, 10 BT-vs-NAV …; also `ChannelAdapter.cpp:2` comment mislabels the space) |

---

## 4. Discrepancy ledger

| ID | Sev | Status | Summary |
|----|-----|--------|---------|
| **D1-800A-NAME** | high (anchor) | PROPOSED | 0x800A named "UPDATE_UI_CONFIG_REPLY" in AASDK vs OAA: request (phone→HU), no reply implied; reply = 0x8012 |
| **D2-8012-MISSING** | high (anchor) | PROPOSED | 0x8012 UpdateHuUiConfigResponse absent from pinned schema + our adapter |
| D3-UICFG-PAYLOAD | medium | PROPOSED | UiConfig{1-4} vs AdditionalVideoConfig{1-8}; f5-8 GAL 4.3+ extensions unrepresentable |
| **D4-SETUP-STATUS** | high | PROPOSED | 0x8003 f1: AASDK WAIT=1/READY=2 vs OAA NONE=0/FAIL=1/OK=2 — value 1 is WAIT-or-FAIL |
| D5-VF-REASON | med-high | PROPOSED | VideoFocusReason 3=LAST_MODE / 4=USER_SELECTION missing in AASDK → proto2 silent default 0 |
| D6-VF-FIELD1 | low | PROPOSED | 0x8007 f1 deprecated disp_channel_id (AASDK) vs "no field 1" (OAA) |
| D7-UNDERFLOW-PAYLOAD | low-med | PROPOSED | 0x800B empty body on wire vs AASDK session_id field |
| D8-SDQ-45-SEMANTICS | medium | PROPOSED | SDP request f4/f5: label_text/device_name (AASDK) vs device_name/device_brand (OAA live capture) |
| D9-SDR-F16 | medium | PROPOSED | SDP response f16 ConnectionConfiguration **retracted** by OAA (GoogleAuth family) |
| D10-STATUS-EXTRA | medium | PROPOSED | Status −26/−27 (OAA) missing in AASDK MessageStatus |
| D11-AUDIOTYPE-NAME | medium | PROPOSED | AudioType 1 GUIDANCE-vs-SPEECH, 4 TELEPHONY-vs-ALARM (OAA unverified) — probe question |
| D12-ORDINAL-FORK | medium | PROPOSED | static ChannelId ordinals fork-drifted (reference space only) |
| D13-NAV14-NAME-REQ | low-med | PROPOSED | 0x000E response-vs-notification naming + required-vs-optional f1 |
| D14-SENTINEL-IDS | low | PROPOSED | 255/65535 internal sentinels under a "wire contract" comment |
| D15-VERSION-RESP-LEN | medium | PROPOSED | VersionResponse ≥v1.6 appends WireConfig; strict 8-byte parse would break |
| **D16-SDP-ORDER** | high | PROPOSED | OAA: HU sends SDP response FIRST; aasdk/ours: request→response. **Probe question #1** |
| D17-OPEN-DIR-CLAIM | medium | PROPOSED | ChannelOpenRequest: OAA proto Phone→HU vs OAA docs HU→Phone. **Probe question #2** |
| D18-CHANNELMAP-CTRL | low | RESOLVED | channel-map control table inverted/wrong content — don't cite it |
| D19-OAA-02-FLAGS | low | RESOLVED | 02-version doc flags 0x00 typo; ours matches architecture table (0x07) |
| D20-VIDFOCUS-OAA-DOCS | low-med | RESOLVED | interactions/04 swaps 0x8007/0x8008; 17.3 ledger + gold protos anchor Phone→HU / HU→phone |
| D21-AUDIOFOCUS-NAME | low | PROPOSED | 0x0012/0x0013 request/response naming |
| D22-VOICE-NAME | low | PROPOSED | 0x0011 request-vs-notification naming |
| D23-SHUTDOWN-NAME | low | PROPOSED | 0x000F/0x0010 shutdown-vs-byebye naming |
| D24-PING-OPTCANON | low | PROPOSED | optional-field canonicalization: our encoder emits present-but-empty `12 00` (PingResponse.f2) where the minimal form is `08 2A`; both wire-legal, bytewise distinct (F-J vs F-J2) |

---

## 5. Fixtures (independent raw-byte spec)

Spec + derivation: `tests/replay/conformance_wire_spec.hpp` on `aa/pre28-conformance-audit`.
Fixture files: `tests/replay/fixtures/conformance/*.json` (task-20 `aa-replay-fixture-v1`
envelope, SHA-256 pinned). Bytes were derived by hand from the cited rulebook sections
and protobuf wire rules — **not** produced by our encoder; tests then compare the
adapter's encode/decode against those literals (round-trip + exact-byte) and assert
fail-closed behavior wherever the spec is strict.

| Fixture | Pins | Adapter assertions |
|---------|------|--------------------|
| F-A version_request_v1_1 | `00 01 00 01 00 01` + control frame `00 07 00 06 …` | encode == literal; decode round-trip; wrong discriminator rejected |
| F-B version_response_v1_7_match | `00 02 00 01 00 07 00 00` | fail-closed today (no response codec; request codec rejects 6-byte body) |
| F-D service_discovery_request_device_info | `00 05` + f4 "Pixel" + f5 "google" | fail-closed today (absent) — locks D8 tag semantics |
| F-E service_discovery_response_minimal_video | `00 06 0A 0E 08 63 1A 0A 08 03 22 06 08 02 10 02 50 03` | encode == literal; decode round-trip |
| F-F channel_open_request_p99_prio1000 | `00 07 08 D0 0F 10 63` (zigzag 1000 = D0 0F) | encode == literal; decode round-trip |
| F-G channel_open_response_success | `00 08 08 00` | encode == literal; round-trip |
| F-H channel_open_response_unsupported | `00 08 08 86 FE FF FF FF FF FF FF FF 01` (status −250: 64-bit varint `86 FE` + 7×`FF` + `01`, 10 bytes) | encode == literal; round-trip |
| F-I ping_request_ts42_bugreport_dead | `00 0B 08 2A 10 01 1A 02 DE AD` | encode == literal; round-trip |
| F-J ping_response_ts42 (spec-minimal) | `00 0C 08 2A` (f2 absent, canonical proto2) | decode == {42, {}} |
| F-J2 ping_response_encoder_form (contested, D24) | `00 0C 08 2A 12 00` (explicit-empty f2) | encode == literal (pins current accepted encoder); decode == {42, {}}; EXPECT_NE vs F-J |
| F-K update_ui_config_request_800A | `80 0A 0A 02 20 01` | spec-lock (absent codec; must never decode as a "reply") |
| F-M update_ui_config_request_8009 | `80 09 0A 02 20 01` | spec-lock (absent codec) |
| F-L update_hu_ui_config_response_8012 | `80 12 08 01` | spec-lock (absent codec; discriminator tests prove current decoders reject) |
| F-N av_setup_request_8000 | `80 00 08 03` | spec-lock (absent codec) |
| F-O av_setup_response_8003_ok | `80 03 08 02` (status 2 = OK/READY, only safe value) | spec-lock |
| F-O2 av_setup_response_8003_contested | `80 03 08 01` (value 1 = WAIT-vs-FAIL, D4) | spec-lock with contested annotation |
| F-P av_start_indication_8001 | `80 01 08 01 10 00 18 01` | spec-lock (GAL 5.0/6.0 fields deliberately absent) |
| F-Q av_stop_indication_8002 | `80 02` (empty body) | spec-lock |
| F-R video_focus_request_8007 | `80 07 10 01 18 03` (reason 3 = LAST_MODE; pins D5 + no-f1) | spec-lock |
| F-S video_focus_indication_8008 | `80 08 08 01 10 00` | spec-lock |
| F-T nav_focus_request_projected | `00 0D 08 02` | spec-lock |

---

## 6. Provenance

- Generated 2026-10-09 for the pre-28 conformance prep package (decision
  `decision-oaa-aasdk-conformance.json`, action items 2-5).
- Companion report: `report-pre28-conformance-2026-10-09.md` / `.json` (same directory).
- No OAA code or schema is compiled into the build; the rulebook is reference-only.
  GAL 4.3-6.0 and modern codecs remain OFF. No accepted task 1-20 behavior was changed
  by this audit except the scoped, behavior-preserving unknown-field separation
  (see report deliverable 3).
