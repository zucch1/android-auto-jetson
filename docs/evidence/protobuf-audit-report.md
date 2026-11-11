# Protobuf Schema Audit Report: AASDK vs Open Android Auto (OAA)

**Schema**: `aa-protobuf-audit-1` | **Audit Date**: 2026-11-11 | **Refs**: [Parent Issue #61](https://github.com/zucch1/android-auto-jetson/issues/61), [Phase 0 Issue #62](https://github.com/zucch1/android-auto-jetson/issues/62)

## 1. Executive Summary

An exhaustive, version-aware, reproducible protobuf audit was conducted comparing all **502** pinned `.proto` files across both upstream repositories:
- **AASDK**: 254 files (`https://github.com/opencardev/aasdk.git@9bf6adf93366`) — 208 messages, 560 fields, 70 enums (790 values).
- **Open Android Auto (OAA)**: 248 files (`https://github.com/mrmees/open-android-auto.git@61eab61c5f99`) — 434 messages, 1032 fields, 121 enums (970 values).

### Upstream Integrity Anchor
- **Completeness**: 100% of all 502 pinned `.proto` files are present with zero missing files.
- **Blob Matching**: 100% of file contents match Git blob identities in `third_party/provenance/{aasdk,oaa}.ls-tree`.
- **Discrepancy Ledger**: **39** findings recorded (all **24** historical legacy findings `D1`–`D24` preserved and re-evaluated against current `main`, plus **15** candidate findings `D25`–`D39`).

### Ledger Summary by Disposition

| Disposition | Count | Meaning |
|---|---|---|
| `confirmed-gap` | **12** | Confirmed wire difference requiring implementation in receiver |
| `deferred` | **4** | Valid finding intentionally deferred to follow-on domain milestone |
| `equivalent` | **9** | Wire representation identical despite naming, syntax, or optional field differences |
| `fixed` | **4** | Resolved on current main commit or architectural design |
| `incorrect-reference` | **5** | Flaw or contradiction in upstream/OAA documentation |
| `intentionally-unsupported` | **1** | Feature out of scope for receiver (e.g. VHAL car control) |
| `not-applicable` | **1** | Obsolete historical protocol channel (e.g. MediaBrowser) |
| `unverified` | **3** | Hypothesis awaiting verification with physical phone capture |

### Ledger Summary by Severity

| Severity | Count |
|---|---|
| `high` | **4** |
| `low` | **13** |
| `low-medium` | **6** |
| `medium` | **13** |
| `medium-high` | **3** |

---

## 2. Re-evaluation of Legacy Discrepancies (D1–D24)

Every discrepancy from the October 9 matrix (`wire-conformance-matrix.json`) was re-evaluated against the current commit on `main`:

| ID | Domain | Sev | Status | Disposition | Current State & Summary |
|---|---|---|---|---|---|
| **D1-800A-NAME** | `av_media_video` | high | proposed | `confirmed-gap` | 0x800A named MEDIA_MESSAGE_UPDATE_UI_CONFIG_REPLY in AASDK vs OAA wire truth (bidirectional request, reply is 0x8012). |
| **D2-8012-MISSING** | `av_media_video` | high | proposed | `confirmed-gap` | 0x8012 UpdateHuUiConfigResponse absent from pinned AASDK schema and adapter. |
| **D3-UICFG-PAYLOAD** | `av_media_video` | medium | proposed | `confirmed-gap` | UiConfig (AASDK f1-4) vs AdditionalVideoConfig (OAA f1-8): fields 5-8 GAL 4.3+ extensions unrepresentable in AASDK. |
| **D4-SETUP-STATUS** | `av_media_video` | high | proposed | `unverified` | 0x8003 body field 1 enum conflict: AASDK Config.Status WAIT=1/READY=2 vs OAA NONE=0/FAIL=1/OK=2. Value 1 is WAIT-or-FAIL. |
| **D5-VF-REASON** | `av_media_video` | medium-high | proposed | `confirmed-gap` | VideoFocusReason enum gap: AASDK has values 0/1/2 only; OAA adds 3=LAST_MODE and 4=USER_SELECTION. |
| **D6-VF-FIELD1** | `av_media_video` | low | proposed | `equivalent` | 0x8007 field 1: AASDK VideoFocusRequestNotification keeps deprecated disp_channel_id vs OAA 'no field 1'. |
| **D7-UNDERFLOW-PAYLOAD** | `av_media_video` | low-medium | proposed | `unverified` | 0x800B AudioUnderflow: 17.3 endpoint receives empty payload vs AASDK session_id field. |
| **D8-SDQ-45-SEMANTICS** | `control` | medium | proposed | `confirmed-gap` | ServiceDiscoveryRequest f4/f5 semantic shift: AASDK label_text/device_name vs OAA device_name/device_brand. |
| **D9-SDR-F16** | `control` | medium | resolved | `incorrect-reference` | ServiceDiscoveryResponse field 16 ConnectionConfiguration retracted by OAA (GoogleAuth family). |
| **D10-STATUS-EXTRA** | `control` | medium | proposed | `confirmed-gap` | Status enum: OAA adds CAR_PROPERTY_SET_FAILED (-26) and CAR_PROPERTY_NOT_AVAILABLE (-27) missing in AASDK MessageStatus. |
| **D11-AUDIOTYPE-NAME** | `audio` | medium | proposed | `equivalent` | AudioType value names: 1 = GUIDANCE (AASDK) vs SPEECH (OAA), 4 = TELEPHONY (AASDK) vs ALARM (OAA). |
| **D12-ORDINAL-FORK** | `control` | medium | resolved | `fixed` | Static ChannelId ordinals fork-drifted between AASDK and GAL service types. |
| **D13-NAV14-NAME-REQ** | `control` | low-medium | resolved | `equivalent` | Control 0x000E: ours/AASDK name it NavFocusNotification with required f1 vs OAA NavFocusResponse with optional f1. |
| **D14-SENTINEL-IDS** | `control` | low | resolved | `fixed` | ControlMessageId::unexpected_message=255 and framing_error=65535 internal sentinels under wire contract comment. |
| **D15-VERSION-RESP-LEN** | `control` | medium | resolved | `fixed` | VersionResponse length is version-conditional: 6-8 raw bytes + >=v1.6 WireConfig tail. |
| **D16-SDP-ORDER** | `control` | high | proposed | `deferred` | Handshake sequencing: OAA documentation claims HU sends SDP response FIRST, before request. |
| **D17-OPEN-DIR-CLAIM** | `control` | medium | resolved | `incorrect-reference` | ChannelOpenRequest direction: OAA proto says Phone->HU vs OAA docs claiming HU->Phone. |
| **D18-CHANNELMAP-CTRL** | `control` | low | resolved | `incorrect-reference` | channel-map.md control table inverts SDP direction. |
| **D19-OAA-02-FLAGS** | `control` | low | resolved | `incorrect-reference` | 02-version doc flags 0x00 typo and misannotated message type. |
| **D20-VIDFOCUS-OAA-DOCS** | `av_media_video` | low-medium | resolved | `incorrect-reference` | interactions/04 AV table swaps 0x8007 and 0x8008 directions. |
| **D21-AUDIOFOCUS-NAME** | `control` | low | proposed | `equivalent` | 0x0012/0x0013 request/response naming: AASDK audio_focus_notification vs OAA AudioFocusResponse. |
| **D22-VOICE-NAME** | `control` | low | proposed | `equivalent` | 0x0011 naming: AASDK voice_session_notification vs OAA VOICE_SESSION_REQUEST. |
| **D23-SHUTDOWN-NAME** | `control` | low | proposed | `equivalent` | 0x000F/0x0010 naming: AASDK byebye_request/response vs OAA SHUTDOWN_REQUEST/RESPONSE. |
| **D24-PING-OPTCANON** | `control` | low | proposed | `equivalent` | PingResponse optional field canonicalization: empty f2 (12 00) vs omitted f2 (08 2A). |

---

## 3. Candidate Findings Beyond the October 9 Audit Scope (D25–D39)

| ID | Domain | Sev | Status | Disposition | Current State & Summary |
|---|---|---|---|---|---|
| **D25-VID-COLOR-SCHEME** | `av_media_video` | medium | proposed | `confirmed-gap` | Video service descriptor field 9 color_scheme_support in OAA AVChannelData.proto, absent in AASDK MediaSinkService.proto. |
| **D26-SEN-EV-ENERGY** | `sensor` | medium | proposed | `confirmed-gap` | Sensor event fields 23-26 in OAA SensorEventIndicationMessage (EV energy, trailer, raw energy, trip settings) absent in AASDK SensorBatch. |
| **D27-WIFI-CRED-STATUS** | `wifi_projection` | medium-high | proposed | `confirmed-gap` | WifiCredentialsResponse field 5: AASDK AccessPointType vs OAA WifiCredentialStatus (0=OK, 1=FAIL), and sequential security mode enum. |
| **D28-INPUT-TS-UNITS** | `input` | medium | proposed | `confirmed-gap` | InputEvent timestamp units: InputAdapter.cpp passes nanoseconds (timestamp_ns) vs wire expectation of microseconds. |
| **D29-AV-MEDIA-STATS-OPTS** | `av_media_video` | low-medium | proposed | `confirmed-gap` | AV media statistics (0x8013) and media options (0x8014) defined in OAA, absent in AASDK MediaMessageId. |
| **D30-AV-OVERLAYS-800D-8010** | `av_media_video` | low-medium | proposed | `deferred` | Integrated overlay messages 0x800D-0x800F and reserved 0x8010 defined in OAA, absent in AASDK. |
| **D31-RADIO-HAL2-GAP** | `radio` | medium | proposed | `deferred` | Radio broadcast architecture divergence: AASDK has 45 protos for Legacy Radio v1 (0x8001-0x8019) vs modern BroadcastRadio HAL 2.0 (0x801A-0x8023). |
| **D32-CAR-CONTROL-SERVICE** | `car_control` | medium | proposed | `intentionally-unsupported` | CarControl service (GAL channel 19, messages 0x8001-0x8007 for VHAL property access) present in OAA, absent in AASDK. |
| **D33-CAR-INTENT-SERVICE** | `car_intent` | low | proposed | `unverified` | CarIntent payload defined in OAA (single optional string metadata field 2, unverified message ID). |
| **D34-MEDIABROWSER-DEAD** | `media_browser` | low | resolved | `not-applicable` | MediaBrowser channel (GAL channel 12, 9 proto files in AASDK): confirmed dead and deprecated in AA 16.1+. |
| **D35-NAV-EV-FORECAST** | `navigation` | low-medium | proposed | `deferred` | Navigation VehicleEnergyForecast message (0x8008 on Navigation channel, double-encoded proto2/proto3 for EV routing) present in OAA. |
| **D36-NAV-MANEUVER-EXT** | `navigation` | low | proposed | `confirmed-gap` | ManeuverType enum extension: OAA adds roundabout and ferry values 43-50; AASDK has values 0-42 plus older roundabout enter/exit 30/31. |
| **D37-BT-PHONE-NAME** | `bluetooth` | low | proposed | `equivalent` | BluetoothPairingRequest adds optional field 3 phone_name in OAA vs 2 fields in AASDK. |
| **D38-MIC-OPEN-PAIR** | `av_media_video` | low | resolved | `equivalent` | MicrophoneOpenResponse field 2 named session_config in OAA vs session_id in AASDK. |
| **D39-UNKNOWN-FIELDS-POLICY** | `control` | medium-high | resolved | `fixed` | Strict unknown-field separation: classify class 1 (unknown enum/wire-type conflict on known tag) vs class 2 (extension tag). |

---

## 4. Prioritized Minimal Implementation Children

Following parent issue #61, confirmed compatibility gaps are partitioned into minimal, independently reviewable child issues:

### Child 1: Control, authentication, service discovery and channel negotiation (`child-1-control-sdp`)

- **Scope**: Reconcile SDP request/response fields, validate SDP sequencing (D16) against physical phone capture, confirm channel open semantics, and enforce unknown-field boundary.
- **Linked Findings**: `D8-SDQ-45-SEMANTICS`, `D9-SDR-F16`, `D10-STATUS-EXTRA`, `D15-VERSION-RESP-LEN`, `D16-SDP-ORDER`, `D17-OPEN-DIR-CLAIM`, `D39-UNKNOWN-FIELDS-POLICY`
- **Required Fixtures**: `F-A (version_request)`, `F-B2 (version_response_tail)`, `F-D (sdp_request_device_info)`, `F-E (sdp_response_minimal)`, `F-F (channel_open_zigzag)`
- **Acceptance Criteria**:
  - [ ] Control adapter correctly decodes ServiceDiscoveryRequest device_name and device_brand fields.
  - [ ] ServiceDiscoveryResponse emits clean descriptor slot set without deprecated field 16.
  - [ ] VersionResponse trailing byte tolerance qualified against v1.6+ phone wire responses.
  - [ ] Physical phone capture on Galaxy S24 / Pixel confirms SDP message order.

### Child 2: AV setup, media, codecs and focus (`child-2-av-media-focus`)

- **Scope**: Resolve AV setup status 1 (D4) with wire evidence, add VideoFocusReason values 3 and 4 (D5), integrate media stats/options IDs (0x8013/0x8014).
- **Linked Findings**: `D4-SETUP-STATUS`, `D5-VF-REASON`, `D6-VF-FIELD1`, `D7-UNDERFLOW-PAYLOAD`, `D20-VIDFOCUS-OAA-DOCS`, `D29-AV-MEDIA-STATS-OPTS`, `D38-MIC-OPEN-PAIR`
- **Required Fixtures**: `F-G (setup_ready)`, `F-H (setup_fail_negative)`, `F-J (video_focus_reason_extended)`, `raw audio underflow fixture`
- **Acceptance Criteria**:
  - [ ] AV sink setup response correctly qualified against phone wire expectations for status 1 vs 2.
  - [ ] VideoFocusNotification decodes reason codes 3 (LAST_MODE) and 4 (USER_SELECTION) without fallback.
  - [ ] Media stats/options message IDs recognized and handled or cleanly ignored without channel tear-down.

### Child 3: Display, video configuration, UI theming and overlays (`child-3-display-ui-theming`)

- **Scope**: Support Material You UI theming (0x8011/0x8012), correct 0x800A request semantics, advertise color_scheme_support in video service descriptor.
- **Linked Findings**: `D1-800A-NAME`, `D2-8012-MISSING`, `D3-UICFG-PAYLOAD`, `D25-VID-COLOR-SCHEME`, `D30-AV-OVERLAYS-800D-8010`
- **Required Fixtures**: `F-K (update_ui_config_request_800A)`, `F-L (update_hu_ui_config_response_8012)`, `theming token payload fixture`
- **Acceptance Criteria**:
  - [ ] Receiver responds with 0x8012 UpdateHuUiConfigResponse upon receiving phone 0x8011 theming tokens.
  - [ ] 0x800A consumed as request direction from phone, never misread as reply.
  - [ ] Video descriptor field 9 color_scheme_support=2 conditionally advertised when UI theming is enabled.

### Child 4: Input, navigation, sensors and vehicle services (`child-4-input-nav-sensors`)

- **Scope**: Correct input timestamp units to microseconds (D28), extend SensorEventIndication to support EV tags 23-26 (D26), add navigation maneuver types 43-50 (D36).
- **Linked Findings**: `D26-SEN-EV-ENERGY`, `D28-INPUT-TS-UNITS`, `D35-NAV-EV-FORECAST`, `D36-NAV-MANEUVER-EXT`
- **Required Fixtures**: `input_report_microsecond_ts`, `sensor_event_ev_energy_tags`, `nav_turn_event_extended_maneuver`
- **Acceptance Criteria**:
  - [ ] InputReport encodes timestamp in elapsedRealtime microseconds.
  - [ ] Sensor sink supports EV energy fields (tags 23-26) when provided by vehicle data source.
  - [ ] Navigation maneuver enum supports extended roundabout and ferry values 43-50.

### Child 5: Wireless projection, Bluetooth, radio and remaining services (`child-5-wireless-bt-radio`)

- **Scope**: Validate Wi-Fi projection credentials field 5 status vs AP-type, evaluate Bluetooth pairing name field, formalize radio HAL 2.0 modernization.
- **Linked Findings**: `D27-WIFI-CRED-STATUS`, `D31-RADIO-HAL2-GAP`, `D32-CAR-CONTROL-SERVICE`, `D37-BT-PHONE-NAME`
- **Required Fixtures**: `wifi_credentials_response_status_0`, `bluetooth_pairing_request_f3`
- **Acceptance Criteria**:
  - [ ] Wi-Fi projection credentials response tag 5 qualified on physical phone connection.
  - [ ] Bluetooth pairing accepts optional phone_name without schema conflict.
  - [ ] Radio architecture decision recorded (HAL 2.0 vs legacy v1).

---

## 5. CI Guard & Verification

Integrity is enforced deterministically by `tools/protocol/protobuf_audit.py --check` and tested by `tests/protocol/audit_scenarios.py` with private mutation fixtures:
1. `baseline`: clean tree passes.
2. `drift-inventory`: any unsynchronized edit to `protobuf-audit-inventory.json` fails with `INVENTORY_DRIFT`.
3. `missing-file`: missing any of the 502 proto files fails with `MISSING_PROTO_FILE`.
4. `tampered-hash`: modifying proto content fails with `CONTENT_HASH_MISMATCH`.
5. `duplicate-id`: duplicated finding ID fails with `DUPLICATE_FINDING_ID`.
6. `missing-field`: omitting mandatory finding attribute fails with `INCOMPLETE_FINDING`.
7. `invalid-disposition`: unknown disposition fails with `INVALID_DISPOSITION`.
8. `unexplained-deletion`: removing any finding fails with `FINDING_DELETED`.

All checks run offline without network access.
