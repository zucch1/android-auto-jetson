// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

namespace aa::wireless_receiver {
// The redacted wireless_admission_expired event. ap_activation: still waiting for the AP;
// bluetooth_link: asked at expiry, the helper reported that the open window's phone has no
// Bluetooth link, so its Android Auto cannot dial the AA RFCOMM profile (the HFP link is owned by
// infotainment; the receiver and helper never page it); phone_handoff: everything else, including
// an unknown answer (a 1.4 helper, a timeout). A link (Device1.Connected) is not proof of HFP.
inline std::string admission_expired_json(bool ap_pending, bool no_link) {
    if (ap_pending)
        return R"json({"event":"wireless_admission_expired","reason":"attempt_deadline","phase":"ap_activation"})json";
    if (no_link)
        return R"json({"event":"wireless_admission_expired","reason":"no_bluetooth_link","phase":"bluetooth_link",)json"
               R"json("detail":"waiting for HFP (owned by infotainment)"})json";
    return R"json({"event":"wireless_admission_expired","reason":"attempt_deadline","phase":"phone_handoff"})json";
}
} // namespace aa::wireless_receiver
