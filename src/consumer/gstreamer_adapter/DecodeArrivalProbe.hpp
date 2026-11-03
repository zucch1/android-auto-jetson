// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "DecodeArrivalObserver.hpp"
#include <gst/gst.h>

namespace aa::consumer {

// Nonblocking data probe, on the appsink sink pad's streaming thread. This
// boundary is sink-pad observed arrival, NOT decoder completion or CPU time.
inline GstPadProbeReturn observe_decode_arrival(GstPad*, GstPadProbeInfo* info,
                                               gpointer data) noexcept {
    auto& observer = *static_cast<DecodeArrivalObserver*>(data);
    const auto now = DecodeArrivalObserver::Clock::now();
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
        const auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (buffer != nullptr && GST_BUFFER_PTS_IS_VALID(buffer)) {
            observer.observe(GST_BUFFER_PTS(buffer), now);
        }
    } else if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) != 0) {
        auto* list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
        if (list != nullptr) {
            for (guint i = 0; i < gst_buffer_list_length(list); ++i) {
                const auto* buffer = gst_buffer_list_get(list, i);
                if (GST_BUFFER_PTS_IS_VALID(buffer)) {
                    observer.observe(GST_BUFFER_PTS(buffer), now);
                }
            }
        }
    }
    return GST_PAD_PROBE_OK;
}

inline constexpr auto kDecodeArrivalProbeTypes = static_cast<GstPadProbeType>(
    GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST);

} // namespace aa::consumer
