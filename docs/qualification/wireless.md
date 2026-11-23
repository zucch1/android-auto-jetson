# Wireless qualification evidence

**Status PENDING.** The 30-minute performance run and two-hour soak on the A26 await [#186](https://github.com/zucch1/android-auto-jetson/issues/186). Fill in `wireless.json` and attach sanitized receipts once the measurements are available.

| Evidence | Result | What to record |
| --- | --- | --- |
| Target and phone | PENDING | Jetson model and software versions, A26 model, Android version, and Android Auto version. |
| Receiver-hosted AP | PENDING | Actual interface and channel, effective per-radio regulatory domain, operator-confirmed jurisdiction, and AP startup timing. |
| BlueZ and enrolment | PENDING | BlueZ version, explicit numeric-comparison acceptance, helper acknowledgement of the bond, persisted approval, and reuse of the approved bond. Omit the comparison digits. |
| Wireless projection | PENDING | RFCOMM and bootstrap completion, authenticated AAP TLS, confirmed ACTIVE, rendered frames, audio writes, and forwarded inputs. |
| 30-minute performance | PENDING | Native live result, continuous AP uptime and helper resource measurements for this run. |
| Two-hour soak | PENDING | Independent native live result, continuous AP uptime and helper resource measurements for this run. |
| Restart and reconnect | PENDING | Receiver restart with consumer re-registration, and link-loss recovery through renegotiation. Verify media and input after both. |
| Limitations | PENDING | Scope limits, unresolved findings, and links to their supporting issues where applicable. |
| Provenance | PENDING | Source, build, configuration, service and target state for the soak configuration. |

The installed-system observations in [#191](https://github.com/zucch1/android-auto-jetson/issues/191) give context for the restart and consumer re-registration checks. Cite the evaluated build and retain the observations supporting each claim. The #191 short projection windows do not supply either #186 performance result.

## Typed manifest specification

Every listed field is required. Objects have exactly the listed keys except the native performance provenance object, whose shape comes from the native report. All text fields must be nonblank and filled in. Use JSON integers for channels and counts, finite JSON numbers for measurements, and JSON booleans for observations. Units appear in field names. Complete the document by setting `schema` to `aa-wireless-qualification/1` and `status` to `COMPLETE`.

| Field | Required shape and acceptance |
| --- | --- |
| `target` | Text fields `model`, `jetpack_version`, `l4t_version`, `kernel_version`. |
| `phone` | Text `model` equal to `Samsung Galaxy A26`, and text fields `android_version`, `android_auto_version`. |
| `ap` | Text `interface`, integer `channel` equal to 36, text `regulatory_domain` and `confirmed_jurisdiction` equal to the same uppercase two-letter country code, `startup_ms` in 0–5000, and a `receipt` reference. Record the AP startup trigger and `AP-ENABLED` endpoint and confirm it holds channel 36 at 5180 MHz. The capability gate's prior timing used driver-load start as its trigger. |
| `bluez` | Text `version`, true `numeric_comparison_accepted`, `helper_bond_acknowledged`, `approval_persisted`, `approved_bond_reused`, and a `receipt` reference. The receipt establishes that explicit comparison acceptance preceded helper bond acknowledgement and persisted approval. |
| `session` | Integer `rfcomm_channel` equal to 8, true `bootstrap_completed`, `aap_tls_authenticated`, `confirmed_active`, strictly positive integers `video_frames`, `audio_writes`, `inputs_forwarded`, and a `receipt` reference. |
| `performance` | Objects `perf_30m` and `soak_2h`, each with `result` and `receipt` references, measured `ap_uptime_seconds` covering the entire active window, measured `helper_rss_peak_mb` greater than 0 and at most 32, and integer `helper_crashes` equal to 0. |
| `recovery` | Objects `receiver_restart` and `session_reconnect`, plus a `receipt` reference. Both attempts require true `confirmed_active`, `video_resumed`, `audio_resumed`, `input_resumed`, and false `fresh_pairing`. For restart, report `elapsed_ms` from the restart request to first repaint in the recovered session, at most 60000. For reconnect, time from entry into reconnecting through transport restoration and renegotiation to confirmed ACTIVE, at most 15000. Both times must be nonnegative. |
| `limitations` | A nonempty array of objects with text `claim` and `detail`. State the specific claim, its scope or missing evidence, and a relevant issue URL if available. Limitations do not waive qualification gates. |
| `provenance` | Copy the native report's provenance object. Both reports must carry the same object, including SHA-256 source/build/config digests, service definitions and digests, dependency versions and target hardware state. The hardware model must match `target.model`. `package_digest` is null and `equivalence_status` is `provisional` for this run; installed package equivalence is tracked in #191. |

Each reference is an object with exactly `path` and `sha256` strings. The path is relative to the manifest directory, and the unprefixed lowercase SHA-256 is calculated after sanitization. Use distinct nonempty regular files for all references. The AP, enrolment, session and recovery receipts should identify the evaluated source/build/config and support the corresponding reported observations.

## Per-run receipt

Each performance `receipt` is a separate JSON file with these four fields.

| Field | Required shape and acceptance |
| --- | --- |
| `generated_at_utc` | Text matching the paired native result. |
| `ap_before`, `ap_after` | Use null when the corresponding native phone Wi-Fi snapshot contains both channel and frequency. If either reading or the whole snapshot is missing, supply an object with integer `channel` equal to 36, integer `frequency_mhz` equal to 5180, text `source` equal to `target-radio` or `phone-wifi`, and text `evidence` recording the independent observation. |
| `resource_observations` | Sanitized text showing helper peak RSS sampling, crash monitoring, and a continuous AP uptime interval spanning the same active window. The native report currently leaves its helper RSS field null. |

The checker compares every available phone-side reading with channel 36 and 5180 MHz. Record fallback readings at the corresponding boundaries of the same run. Use the active radio's observed channel and frequency or a separate phone Wi-Fi status reading. Include the time, source and sanitized readings in `evidence`. Operator configuration alone cannot corroborate a missing reading, and an available contradictory reading fails even when the receipt claims channel 36.


## Performance gates

Retain the native `aa-projection-perf/3` result from each `aa-projection-perf.sh --mode wireless` run. Remove its optional raw capture pointer before export. Use the native live results for the 30m and 2h durations (or the soak chain’s equivalent second values), with complete active measurement coverage, distinct timestamps, a verified Release build, and AP settings matching the manifest. Wireless-only evidence includes bound ADB and clean host USB readings before, during and after. The native report also supplies the thermal samples, moving-map workload and zero receiver crashes. Use the same thermal policy for both runs. Wireless startup wait must be nonnegative and within the collector’s duration-dependent startup ceiling, which reserves the active window and preparation headroom within the receiver lifetime.

- Receiver RSS peak at most 250 MB, RSS growth strictly below 10%, sustained receiver CPU at most 50% of one core.
- Audio path latency at most 200 ms.
- A/V absolute p99 at most 50 ms, absolute worst 5-second mean at most 40 ms, and valid audio coverage at least 90%. The signed soak rule permits at most one pair with absolute skew above 90 ms per 30 minutes of active measurement, using the native per-stage allowance. No pair may exceed 150 ms. The export must retain every excursion's time and signed skew.
- Record thermal state, continuous AP uptime, helper peak RSS and helper crashes for each active window.

The A/V measurements use software endpoints. Record the physical A/V, HFP-call and other release scope limits separately in the limitations register where applicable. A missed gate fails full compliance. Any owner-signed withdrawal of claims belongs in the capability/readiness decision trail.

## Validate the export

Keep raw captures and credentials outside the export. Review every exported file for phone identifiers, serials, hardware addresses, SSIDs, PSKs and other private data. Omit these values or use the documented redaction forms. The privacy checker detects known formats and labelled fields. Review free text and filenames before publication.

```sh
python3 -B tools/qualification/check_wireless.py docs/qualification/wireless.json
python3 -B tools/qualification/check_privacy.py docs/qualification
```

The wireless checker exits 0 on a complete passing package, 1 on pending, missing, invalid or unsafe evidence, and 2 on command-line usage errors. It checks evidence structure, integrity and recorded measurements. Hardware observations still require review of the sanitized receipts. The committed PENDING template exits 1.
