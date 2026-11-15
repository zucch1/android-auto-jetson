// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reference wired-enrolment consumer flow (#133), kept free of the bus and
// the terminal so it is unit-tested with fakes. The real consumer (the
// infotainment UI) drives the same contract calls in the same order:
//
//   RegisterConsumer -> RequestAddPhone -> GetPairingCandidate
//   -> show the redacted label, ask a human yes/no
//   -> yes: ConfirmPhonePairing, then CancelPhonePairing (closes the window;
//      the approval is already durable)    no/timeout: CancelPhonePairing
//
// Only an explicit "y"/"yes" approves. Anything else, an empty line, EOF or
// no answer before the deadline is a no.

#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/ipc/PeerCredentials.hpp>

#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <string>

namespace aa::tools::enrol {

enum class Outcome { approved = 0, declined = 1, failed = 3 };

// Leave headroom inside the receiver's 60 s confirm window for the round trip.
inline constexpr core::Milliseconds kAnswerBudget{50'000};

[[nodiscard]] inline bool explicit_yes(std::string answer) {
    while (!answer.empty() && std::isspace(static_cast<unsigned char>(answer.back()))) answer.pop_back();
    std::size_t start = 0;
    while (start < answer.size() && std::isspace(static_cast<unsigned char>(answer[start]))) ++start;
    answer.erase(0, start);
    for (char& c : answer) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return answer == "y" || answer == "yes";
}

// ReadAnswer: one line from the human within the budget, nullopt on EOF or timeout.
using ReadAnswer = std::function<std::optional<std::string>(core::Milliseconds)>;

template <class Client>
[[nodiscard]] Outcome run(Client& client, const ipc::BusName& consumer_name, std::ostream& out,
                          const ReadAnswer& read_answer) {
    if (const auto registered = client.register_consumer(consumer_name); !registered) {
        out << "error: could not register as the receiver consumer ("
            << to_string(registered.error().code()) << ")\n";
        return Outcome::failed;
    }
    const auto request = client.request_add_phone();
    if (!request) {
        out << "error: no refused wired phone to enrol ("
            << to_string(request.error().code()) << ")\n"
            << "plug the phone in, wait for the receiver to refuse it, then retry\n";
        (void)client.unregister_consumer();
        return Outcome::failed;
    }
    const auto candidate = client.pairing_candidate(request.value());
    if (!candidate) {
        out << "error: request " << request.value().value << " has no candidate ("
            << to_string(candidate.error().code()) << ")\n";
        (void)client.cancel_phone_pairing(request.value());
        (void)client.unregister_consumer();
        return Outcome::failed;
    }
    const auto field = [&](const char* name) {
        const auto found = candidate.value().find(name);
        return found == candidate.value().end() ? std::string{"?"} : found->second;
    };
    out << "Phone refused by the receiver:\n"
        << "  transport: " << field("transport") << "\n"
        << "  label:     " << field("label") << "\n"
        << "  strength:  " << field("strength") << "\n"
        << "The label matches the phone_id in the receiver's refusal log line.\n"
        << "Approve this phone? [y/N] " << std::flush;
    const auto answer = read_answer(kAnswerBudget);
    if (!answer || !explicit_yes(*answer)) {
        out << (answer ? "\n" : "\nno answer; ") << "not approved\n";
        (void)client.cancel_phone_pairing(request.value());
        (void)client.unregister_consumer();
        return Outcome::declined;
    }
    const auto confirmed = client.confirm_phone_pairing(request.value());
    if (!confirmed) {
        out << "error: approval refused (" << to_string(confirmed.error().code()) << ")\n";
        (void)client.cancel_phone_pairing(request.value());
        (void)client.unregister_consumer();
        return Outcome::failed;
    }
    (void)client.cancel_phone_pairing(request.value());  // close the pairing window
    (void)client.unregister_consumer();
    out << "approved: the receiver admits this phone on its next attempt\n";
    return Outcome::approved;
}

} // namespace aa::tools::enrol
