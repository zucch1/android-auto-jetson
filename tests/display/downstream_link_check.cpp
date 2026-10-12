// SPDX-License-Identifier: GPL-3.0-or-later
// Build-only downstream link check (PR #36 fix round). It proves that a
// project linking ONLY through aareceiver-display resolves the complete
// display session logic in EVERY configuration - with the consumer decode
// helper present AND with it explicitly unavailable (the no-overlay cross
// build). The failure it guards against: aareceiver-display compiled with
// helper_decode_sessions() referencing consumer::DecodeHelper symbols that no
// archive resolves when aareceiver-consumer is absent, so the downstream link
// broke instead of the library's own.
//
// The AArch64 cross binary is NEVER executed - its successful link (and its
// AArch64 ELF class) is the assertion. main() references the decode factory
// and a controller API so the archive objects pull in, and encodes the
// expected typed channel_decode_unavailable for the no-consumer case (never a
// missing symbol, never an untyped failure).
#include <aa/display/Display.hpp>

int main() {
    // Reference the production factory and a controller API through
    // aareceiver-display only - this pulls DisplayController.o in.
    const aa::display::DecodeSessionFactory factory = aa::display::helper_decode_sessions();
    auto* const controller_api = &aa::display::DisplayController::attach;
    (void)controller_api;

    // Expected in the consumer-unavailable configuration: a typed
    // channel_decode_unavailable from the rejecting factory. A consumer-present
    // configuration may open the real helper; anything else is a broken link
    // contract.
    const auto opened = factory(aa::consumer::DecodeConfig{});
    if (opened.has_value()) {
        return 0;
    }
    return opened.error().code() == aa::ErrorCode::channel_decode_unavailable ? 0 : 1;
}
