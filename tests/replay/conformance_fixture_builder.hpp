// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Builders that wrap the independent conformance wire spec
// (tests/replay/conformance_wire_spec.hpp) into task-20 aa-replay-fixture-v1
// records via the replay Recorder. The envelope is container plumbing; every
// payload/frame byte comes from the hand-derived spec, never from our encoder.
// Used by both aa_conformance_fixture_write (artifact generation) and the
// conformance tests (byte-identity + SHA-256 pinning).

#include "conformance_wire_spec.hpp"

#include <aa/replay/Recorder.hpp>
#include <aa/replay/Schema.hpp>

#include <string_view>

namespace conformancewire {

namespace replay = aa::replay;

inline void set_common_metadata(replay::Recorder& recorder, std::string_view case_name) {
    (void)recorder.set_metadata(aa::diagnostics::EventField::text("subject", "wire-conformance"));
    (void)recorder.set_metadata(
        aa::diagnostics::EventField::text("case", std::string{case_name}));
    (void)recorder.set_metadata(
        aa::diagnostics::EventField::text("provenance", "oaa-61eab61c-hand-derived"));
}

inline void expect_minimal(replay::Recorder& recorder, std::int64_t sends) {
    replay::Expectation expect;
    expect.states = {"disconnected"};
    expect.sends = sends;
    (void)recorder.set_expectation(expect);
}

// F-A/F-B/F-D/F-E/F-F/F-G/F-I/F-J: control-channel handshake byte set.
inline void fill_control(replay::Recorder& recorder) {
    using aa::core::Nanoseconds;
    set_common_metadata(recorder, "conformance-control");
    expect_minimal(recorder, 4); // outbound: F-A, F-E, F-G, F-J
    (void)recorder.record_transport(Nanoseconds{1'000'000}, replay::Direction::outbound,
                                    F_A_version_request_v1_1_frame);
    (void)recorder.record_transport(Nanoseconds{2'000'000}, replay::Direction::inbound,
                                    F_B_version_response_v1_7_frame);
    (void)recorder.record_transport(Nanoseconds{3'000'000}, replay::Direction::outbound,
                                    F_E_service_discovery_response_frame);
    (void)recorder.record_transport(Nanoseconds{4'000'000}, replay::Direction::inbound,
                                    F_D_service_discovery_request_frame);
    (void)recorder.record_transport(Nanoseconds{5'000'000}, replay::Direction::inbound,
                                    F_F_channel_open_request_frame);
    (void)recorder.record_transport(Nanoseconds{6'000'000}, replay::Direction::outbound,
                                    F_G_channel_open_response_success_frame);
    (void)recorder.record_transport(Nanoseconds{7'000'000}, replay::Direction::inbound,
                                    F_I_ping_request_frame);
    (void)recorder.record_transport(Nanoseconds{8'000'000}, replay::Direction::outbound,
                                    F_J_ping_response_frame);
}

// F-N/F-O/F-O2/F-P/F-Q/F-R/F-S: AV lifecycle byte set on the dynamic video
// service byte (0x63 = 99).
inline void fill_media(replay::Recorder& recorder) {
    using aa::core::Nanoseconds;
    set_common_metadata(recorder, "conformance-media");
    expect_minimal(recorder, 3); // outbound: F-O, F-O2 (contested), F-S
    (void)recorder.record_transport(Nanoseconds{1'000'000}, replay::Direction::inbound,
                                    F_N_av_setup_request_8000_frame);
    (void)recorder.record_transport(Nanoseconds{2'000'000}, replay::Direction::outbound,
                                    F_O_av_setup_response_8003_ok_frame);
    (void)recorder.record_transport(Nanoseconds{3'000'000}, replay::Direction::inbound,
                                    F_P_av_start_indication_8001_frame);
    (void)recorder.record_transport(Nanoseconds{4'000'000}, replay::Direction::inbound,
                                    F_Q_av_stop_indication_8002_frame);
    (void)recorder.record_transport(Nanoseconds{5'000'000}, replay::Direction::inbound,
                                    F_R_video_focus_request_8007_frame);
    (void)recorder.record_transport(Nanoseconds{6'000'000}, replay::Direction::outbound,
                                    F_S_video_focus_indication_8008_frame);
    (void)recorder.record_transport(Nanoseconds{7'000'000}, replay::Direction::outbound,
                                    F_O2_av_setup_response_8003_contested_frame);
}

// F-M/F-K/F-L: UI-config anchor byte set (0x8009/0x800A/0x8012).
inline void fill_uiconfig(replay::Recorder& recorder) {
    using aa::core::Nanoseconds;
    set_common_metadata(recorder, "conformance-uiconfig");
    expect_minimal(recorder, 2); // outbound: F-M (0x8009), F-L (0x8012)
    (void)recorder.record_transport(Nanoseconds{1'000'000}, replay::Direction::outbound,
                                    F_M_update_ui_config_request_8009_frame);
    (void)recorder.record_transport(Nanoseconds{2'000'000}, replay::Direction::inbound,
                                    F_K_update_ui_config_request_800A_frame);
    (void)recorder.record_transport(Nanoseconds{3'000'000}, replay::Direction::outbound,
                                    F_L_update_hu_ui_config_response_8012_frame);
}

using Filler = void (*)(replay::Recorder&);

inline aa::core::Result<replay::ExportedFixture> build(Filler filler) {
    replay::Recorder recorder;
    filler(recorder);
    return recorder.export_fixture();
}

} // namespace conformancewire
