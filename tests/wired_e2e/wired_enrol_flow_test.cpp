// SPDX-License-Identifier: GPL-3.0-or-later
// #133 reference consumer (aa-wired-enrol) flow: only an explicit "y"/"yes"
// confirms; everything else cancels; the request is always closed.
#include "../../tools/wired-enrol/AnswerReader.hpp"
#include "../../tools/wired-enrol/EnrolFlow.hpp"

#include <unistd.h>

#include <chrono>

#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace enrol = aa::tools::enrol;
using aa::Error;
using aa::ErrorCode;

struct FakeClient final {
    std::vector<std::string> calls{};
    aa::core::Result<aa::core::PairingRequestId> request{aa::core::PairingRequestId{77}};
    aa::core::Result<void> confirm{};

    aa::core::Result<aa::core::ConsumerId> register_consumer(const aa::ipc::BusName&) {
        calls.emplace_back("register");
        return aa::core::ConsumerId{1};
    }
    aa::core::Result<void> unregister_consumer() { calls.emplace_back("unregister"); return {}; }
    aa::core::Result<aa::core::PairingRequestId> request_add_phone() {
        calls.emplace_back("request");
        return request;
    }
    aa::core::Result<std::map<std::string, std::string>> pairing_candidate(aa::core::PairingRequestId) {
        calls.emplace_back("candidate");
        return std::map<std::string, std::string>{
            {"transport", "wired"}, {"label", "id-1234"}, {"strength", "serial-backed"}};
    }
    aa::core::Result<void> confirm_phone_pairing(aa::core::PairingRequestId) {
        calls.emplace_back("confirm");
        return confirm;
    }
    aa::core::Result<void> cancel_phone_pairing(aa::core::PairingRequestId) {
        calls.emplace_back("cancel");
        return {};
    }
};

enrol::ReadAnswer answer(std::optional<std::string> value) {
    return [value](aa::core::Milliseconds) { return value; };
}

TEST(WiredEnrolFlow, ExplicitYesConfirmsThenClosesTheWindow) {
    for (const char* yes : {"y", "Y", "yes", " YES \n"}) {
        FakeClient client;
        std::ostringstream out;
        EXPECT_EQ(enrol::run(client, aa::ipc::BusName{"org.custom.WiredEnrol1"}, out, answer(yes)),
                  enrol::Outcome::approved);
        EXPECT_EQ(client.calls, (std::vector<std::string>{"register", "request", "candidate",
                                                          "confirm", "cancel", "unregister"}));
        EXPECT_NE(out.str().find("id-1234"), std::string::npos);
        EXPECT_NE(out.str().find("[y/N]"), std::string::npos);
    }
}

TEST(WiredEnrolFlow, AnythingButAnExplicitYesDeclines) {
    for (const std::optional<std::string>& no :
         {std::optional<std::string>{""}, std::optional<std::string>{"n"},
          std::optional<std::string>{"yess"}, std::optional<std::string>{"ok"},
          std::optional<std::string>{}}) {
        FakeClient client;
        std::ostringstream out;
        EXPECT_EQ(enrol::run(client, aa::ipc::BusName{"x.y"}, out, answer(no)),
                  enrol::Outcome::declined);
        EXPECT_EQ(client.calls, (std::vector<std::string>{"register", "request", "candidate",
                                                          "cancel", "unregister"}));
    }
}

TEST(WiredEnrolFlow, NoRefusedPhoneAndRefusedApprovalFail) {
    FakeClient none;
    none.request = Error{ErrorCode::trust_unknown_phone};
    std::ostringstream out;
    EXPECT_EQ(enrol::run(none, aa::ipc::BusName{"x.y"}, out, answer("y")), enrol::Outcome::failed);
    EXPECT_EQ(none.calls, (std::vector<std::string>{"register", "request", "unregister"}));

    FakeClient refused;
    refused.confirm = Error{ErrorCode::trust_rejected};
    EXPECT_EQ(enrol::run(refused, aa::ipc::BusName{"x.y"}, out, answer("y")), enrol::Outcome::failed);
    EXPECT_EQ(refused.calls.back(), "unregister");
}

TEST(WiredEnrolFlow, NotDurableApprovalIsAFailureNotApproved) {
    FakeClient client;
    client.confirm = Error{ErrorCode::trust_store_not_durable};
    std::ostringstream out;
    EXPECT_EQ(enrol::run(client, aa::ipc::BusName{"x.y"}, out, answer("y")), enrol::Outcome::failed);
    EXPECT_NE(out.str().find("trust_store_not_durable"), std::string::npos) << out.str();
    EXPECT_EQ(out.str().find("approved:"), std::string::npos) << out.str();
    EXPECT_EQ(client.calls.back(), "unregister");
}

TEST(WiredEnrolFlow, RunPassesTheInjectedBudgetToTheReader) {
    FakeClient client;
    std::ostringstream out;
    aa::core::Milliseconds seen{};
    EXPECT_EQ(enrol::run(client, aa::ipc::BusName{"x.y"}, out,
                         [&seen](aa::core::Milliseconds budget) {
                             seen = budget;
                             return std::optional<std::string>{};
                         },
                         aa::core::Milliseconds{250}),
              enrol::Outcome::declined);
    EXPECT_EQ(seen.count, 250);
}

// #141 B3: the answer read is bounded by one deadline even when the writer
// sends a partial line and keeps the pipe open.
struct Pipe final {
    int fds[2]{-1, -1};
    Pipe() { EXPECT_EQ(::pipe(fds), 0); }
    ~Pipe() {
        for (int fd : fds) if (fd >= 0) ::close(fd);
    }
    void write(std::string_view text) const {
        EXPECT_EQ(::write(fds[1], text.data(), text.size()), static_cast<ssize_t>(text.size()));
    }
    void close_writer() { ::close(fds[1]); fds[1] = -1; }
};

TEST(WiredEnrolAnswerReader, PartialLineOnAnOpenPipeTimesOut) {
    Pipe pipe;
    pipe.write("y");
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(enrol::read_answer_line(pipe.fds[0], aa::core::Milliseconds{200}).has_value());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GE(elapsed, std::chrono::milliseconds{150});
    EXPECT_LT(elapsed, std::chrono::seconds{2});
}

TEST(WiredEnrolAnswerReader, CompleteLinesEofAndOverlongInput) {
    {
        Pipe pipe;
        pipe.write("ye");
        pipe.write("s\nignored");
        const auto line = enrol::read_answer_line(pipe.fds[0], aa::core::Milliseconds{1000});
        ASSERT_TRUE(line.has_value());
        EXPECT_EQ(*line, "yes");
    }
    {
        Pipe pipe;  // EOF before a newline is no answer (a "no")
        pipe.write("y");
        pipe.close_writer();
        EXPECT_FALSE(enrol::read_answer_line(pipe.fds[0], aa::core::Milliseconds{1000}).has_value());
    }
    {
        Pipe pipe;  // more than the line bound without a newline declines at once
        pipe.write(std::string(enrol::kMaxAnswerBytes + 8, 'y'));
        const auto started = std::chrono::steady_clock::now();
        EXPECT_FALSE(enrol::read_answer_line(pipe.fds[0], aa::core::Milliseconds{5000}).has_value());
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{2});
    }
}

} // namespace
