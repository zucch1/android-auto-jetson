// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/input/InputService.hpp>

#include <cstdint>
#include <vector>

namespace aa::protocol {

// Phone-facing AAP input encoding (finding F2). The phone expects an
// InputReport protobuf (pinned AASDK schema, InputMessageId INPUT_MESSAGE_INPUT_REPORT
// = 0x8001) framed with its 2-byte big-endian input message id on the input
// service channel - NOT the local 24-byte IPC record. This is the boundary that
// converts a channel-normalized event into that AAP wire payload. Defined in
// the AASDK adapter (src/protocol/aasdk_adapter/); no protobuf type crosses this
// boundary. `timestamp_ns` fills InputReport.timestamp (monotonic A/V clock).
[[nodiscard]] core::Result<std::vector<std::byte>>
encode_input_report(const input::NormalizedInput &event, std::uint64_t timestamp_ns);

} // namespace aa::protocol
