// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/diagnostics/Diagnostics.hpp>
#include <aa/replay/Fixture.hpp>
#include <aa/replay/Schema.hpp>

#include <cstddef>
#include <set>
#include <span>
#include <vector>

namespace aa::replay {

// ONE bounded recorder (task 20). Typed capture methods only - there is no
// transport decorator recorder. Records are caller-supplied synthetic data;
// the recorder never reads a live link, which is the documented synthetic-only
// limitation (raw local capture is out of scope and nothing here claims
// arbitrary binary capture is privacy-safe).
//
// Bounds: record count, per-kind payload bytes, total payload bytes and trace
// duration are capped (Fixture.hpp); overrunning any of them is a typed error
// and no partial state is kept past the failed call. Timestamps must be
// nondecreasing monotonic nanoseconds.
//
// Export runs the redaction/export gate: metadata crosses the task-19
// diagnostics serializer (to_json) with credentials/locations redacted and
// identifiers pseudonymized then fixed-redacted, and the fixture is sealed
// with a SHA-256 over the fixed canonical bytes. Opaque record payloads are
// stored verbatim under the explicit synthetic opaque policy.
class Recorder final {
public:
    Recorder() = default;
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    Recorder(Recorder&&) = delete;
    Recorder& operator=(Recorder&&) = delete;

    // Typed metadata for the fixture header. Values may be raw; export
    // enforces the diagnostics privacy boundary. Identifiers should be
    // declared as such (EventField::identifier) so they pseudonymize.
    core::Result<void> set_metadata(const diagnostics::EventField& field);

    // Replaces the services snapshot (validated on export by the real
    // task-18 validate_service path).
    core::Result<void> set_services(std::vector<ServiceEntry> services);

    core::Result<void> record_session(core::Nanoseconds t, const SessionRecord& step);
    core::Result<void> record_transport(core::Nanoseconds t, Direction dir,
                                        std::span<const std::byte> frame);
    core::Result<void> record_video(core::Nanoseconds t, const VideoRecord& video);
    core::Result<void> record_audio(core::Nanoseconds t, const AudioRecord& audio);
    core::Result<void> record_input(core::Nanoseconds t, const InputRecord& input);

    // Expected output trace to seal into the fixture (transitions/outcomes).
    core::Result<void> set_expectation(Expectation expected);

    // Builds the fixture and exports it through the redaction/export gate.
    [[nodiscard]] core::Result<ExportedFixture> export_fixture() const;

    // The in-memory fixture (pre-export, raw metadata). Tests use this to
    // drive the player without a file round-trip; export is the gate.
    [[nodiscard]] const Fixture& fixture() const noexcept { return fixture_; }

private:
    [[nodiscard]] core::Result<void> admit(core::Nanoseconds t, std::size_t payload_bytes);
    [[nodiscard]] std::set<protocol::ChannelRole> service_roles() const;

    Fixture fixture_{};
    std::vector<diagnostics::EventField> metadata_fields_{};
    AdmissionBudget budget_{};
};

} // namespace aa::replay
