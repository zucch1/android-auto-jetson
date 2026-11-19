// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

namespace aa::wireless_receiver {
// The redacted wireless_admission_expired event. ap_activation: still waiting for the AP;
// bluetooth_link: the helper reported no Bluetooth link to the phone, so its Android Auto cannot
// dial the AA RFCOMM profile (the HFP link is owned by infotainment; the receiver and helper never
// page it); phone_handoff: AP up and a link, but no phone session (bootstrap, AP join or TCP).
inline std::string admission_expired_json(bool ap_pending, bool no_link) {
    if (ap_pending)
        return R"json({"event":"wireless_admission_expired","reason":"attempt_deadline","phase":"ap_activation"})json";
    if (no_link)
        return R"json({"event":"wireless_admission_expired","reason":"no_bluetooth_link","phase":"bluetooth_link",)json"
               R"json("detail":"waiting for HFP (owned by infotainment)"})json";
    return R"json({"event":"wireless_admission_expired","reason":"attempt_deadline","phase":"phone_handoff"})json";
}
} // namespace aa::wireless_receiver
