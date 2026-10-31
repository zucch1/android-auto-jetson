// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual harness adapters, not copies of their logging logic.
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
// This state is private to the actual-main test inclusion. No callback registry
// lock is involved, and lock() never waits on the real mutex with state held.
struct ShutdownObservation {
    using Clock = std::chrono::steady_clock;
    struct Event {
        std::string name;
        std::thread::id thread;
    };
    std::mutex state;
    std::condition_variable cv;
    std::vector<Event> trace;
    std::thread::id owner;
    std::thread::id dispatcher;
    std::thread::id reset_owner;
    Clock::time_point published_at{};
    bool published = false;
    bool receive_held = false;
    bool release_receive = false;
    bool release_main = false;
    bool cleanup_held = false;
    bool release_cleanup = false;
    bool main_attempt = false;
    bool unprotected_stop = false;
    bool unprotected_during_cleanup = false;
    bool stop_owned = false;
    bool join_owned = false;
    bool reset_ok = false;
    bool gate_failed = false;
    bool closed = false;
    int resets = 0;
    int stops = 0;
    int joins = 0;
    int pings = 0;
    int replies = 0;
    int final_receives = 0;
    int post_close_io = 0;

    void record(const char *name) {
        trace.push_back({name, std::this_thread::get_id()});
        cv.notify_all();
    }
    void release_all() {
        release_receive = release_main = release_cleanup = true;
        cv.notify_all();
    }
    template <class Predicate>
    bool wait(std::unique_lock<std::mutex> &lock, Predicate predicate) {
        if (cv.wait_for(lock, std::chrono::seconds{2}, predicate)) {
            return true;
        }
        gate_failed = true;
        release_all();
        return false;
    }
    void published_false() {
        std::unique_lock lock(state);
        dispatcher = std::this_thread::get_id();
        published_at = Clock::now();
        published = true;
        record("false");
        (void)wait(lock, [&] { return release_main; });
    }
    void main_stop() {
        std::unique_lock lock(state);
        ++stops;
        stop_owned = owner == std::this_thread::get_id();
        if (!stop_owned) {
            unprotected_stop = true;
            unprotected_during_cleanup = cleanup_held && owner == reset_owner;
            record("MAIN_STOP_WITHOUT_SESSION_MUTEX");
            // The mutant proves the invariant violation, never a data race.
            // Release all gates on failure; do not access the source until reset.
            if (!wait(lock, [&] { return resets == 1; }) &&
                !wait(lock, [&] { return resets == 1; })) {
                std::abort();
            }
        }
        record("stop");
    }
    void main_join() {
        const std::lock_guard lock(state);
        ++joins;
        join_owned = owner == std::this_thread::get_id();
        record("join");
    }
    void reset_finished(bool ok) {
        const std::lock_guard lock(state);
        ++resets;
        reset_ok = ok && owner == std::this_thread::get_id();
        record("reset");
    }
    void io() {
        const std::lock_guard lock(state);
        if (closed) {
            ++post_close_io;
        }
    }
    void cleanup_close() {
        std::unique_lock lock(state);
        closed = true;
        cleanup_held = true;
        reset_owner = std::this_thread::get_id();
        record("close");
        (void)wait(lock, [&] { return release_cleanup; });
        cleanup_held = false;
    }
};
ShutdownObservation *shutdown_observation = nullptr;

// Actual-main scheduling seam, absent from the shipped executable. Waiting to
// the real cutoff makes the ordering deterministic, not a scheduler sleep race.
struct DeadlineObservation {
    enum class Edge { dispatch, accepted, owner };
    Edge edge;
    std::chrono::steady_clock::time_point cutoff{};
    std::mutex mutex;
    std::condition_variable cv;
    bool crossed = false;
    bool owner_done = false;
    bool gate_failed = false;

    explicit DeadlineObservation(Edge value) : edge(value) {}
    void cross(Edge point) {
        if (edge == point) {
            std::this_thread::sleep_until(cutoff);
            const std::lock_guard lock(mutex);
            crossed = std::chrono::steady_clock::now() >= cutoff;
        }
    }
    void finish_owner() {
        const std::lock_guard lock(mutex);
        owner_done = true;
        cv.notify_all();
    }
    void hold_dispatcher() {
        if (edge == Edge::owner) {
            std::unique_lock lock(mutex);
            gate_failed = !cv.wait_for(lock, std::chrono::seconds{3}, [&] { return owner_done; });
        }
    }
};
DeadlineObservation *deadline_observation = nullptr;
struct RequiredFailureObservation {
    bool negotiation_timeout = false;
};
RequiredFailureObservation *required_failure_observation = nullptr;

class ObservedSessionMutex {
  public:
    void lock() {
        if (shutdown_observation) {
            const std::lock_guard lock(shutdown_observation->state);
            if (shutdown_observation->published &&
                shutdown_observation->dispatcher == std::this_thread::get_id()) {
                shutdown_observation->main_attempt = true;
            }
            shutdown_observation->record("attempt");
        }
        mutex_.lock();
        if (shutdown_observation) {
            const std::lock_guard lock(shutdown_observation->state);
            shutdown_observation->owner = std::this_thread::get_id();
            shutdown_observation->record("acquire");
        }
    }
    void unlock() {
        if (shutdown_observation) {
            const std::lock_guard lock(shutdown_observation->state);
            shutdown_observation->record("release");
            shutdown_observation->owner = {};
            mutex_.unlock();
        } else {
            mutex_.unlock();
        }
    }
  private:
    std::mutex mutex_;
};
} // namespace

#define AA_WIRED_RECEIVER_SHUTDOWN_TEST
#define main wired_receiver_main
#include "../../tools/wired-receiver/main.cpp" // NOLINT(bugprone-suspicious-include)
#undef main
#undef AA_WIRED_RECEIVER_SHUTDOWN_TEST

#include "harness.hpp"
#include "../session/Fakes.hpp"
#include "../video_socket/pair.hpp"
#include "../trust/syscall_faults.hpp"
#include "../transport/UsbAoaFakes.hpp"
#include "../../src/transport/usb_adapter/LibusbWorld.hpp"
#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <gtest/gtest.h>
#include <array>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <pthread.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sstream>
#include <unistd.h>
#include <poll.h>
#include <sys/syscall.h>
#include <spawn.h>

namespace {
namespace media_wire = aap_protobuf::service::media::sink::message;

using LogClock = std::chrono::steady_clock;
using ReceiverLogScope = aa::wired_receiver::StdoutLogScope;
void flush_receiver_stdout() {
    if (auto *log = aa::wired_receiver::active_stdout_log.load()) { EXPECT_FALSE(log->flush()); }
}
void capture_receiver_stdout() {
    flush_receiver_stdout();
    testing::internal::CaptureStdout();
}
std::string captured_receiver_stdout() {
    flush_receiver_stdout();
    return testing::internal::GetCapturedStdout();
}
bool log_readable(int fd, std::chrono::seconds budget = std::chrono::seconds{5}) {
    const auto deadline = LogClock::now() + budget;
    for (;;) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - LogClock::now());
        if (left.count() <= 0) { return false; }
        pollfd item{fd, POLLIN, 0};
        const int result = ::poll(&item, 1, static_cast<int>(left.count()));
        if (result < 0 && errno == EINTR) { continue; }
        return result > 0 && (item.revents & POLLIN) != 0;
    }
}

// The first stdout read releases the gate, including on assertion unwinding.
struct LogChild {
    int output[2]{-1, -1};
    int control[2]{-1, -1};
    int exited = -1;
    pid_t pid = -1;
    int status = -1;
    bool forced = false;
    bool eof = false;
    bool io_ok = true;
    std::string bytes;
    static void close_fd(int &fd) {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    void finish() {
        if (pid <= 0) { return; }
        close_fd(output[1]);
        close_fd(control[1]);
        const auto deadline = LogClock::now() + std::chrono::seconds{5};
        std::array<char, 4096> buffer{};
        while (!eof && LogClock::now() < deadline) {
            const auto count = ::read(output[0], buffer.data(), buffer.size());
            if (count > 0) { bytes.append(buffer.data(), static_cast<std::size_t>(count)); continue; }
            if (count == 0) { eof = true; break; }
            if (errno == EINTR) { continue; }
            if (errno != EAGAIN) { io_ok = false; break; }
            pollfd item{output[0], POLLIN, 0};
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - LogClock::now());
            if (left.count() > 0) { (void)::poll(&item, 1, static_cast<int>(left.count())); }
        }
        if (exited < 0 || !log_readable(exited)) {
            forced = true;
            (void)::kill(pid, SIGKILL);
        }
        pid_t waited;
        do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
        io_ok = io_ok && waited == pid;
        pid = -1;
    }
    ~LogChild() {
        finish();
        for (auto &fd : output) { close_fd(fd); }
        for (auto &fd : control) { close_fd(fd); }
        close_fd(exited);
    }
};

TEST(WiredReceiver, LogLineReturnsBeforeFullStdoutGateReleaseLosslessly) {
    // Given: a fresh filtered child process forks before starting a writer.
    if (!std::getenv("AA_LOG_PIPE_CHILD")) {
        std::string executable = "/proc/self/exe";
        std::string filter = "--gtest_filter=WiredReceiver.LogLineReturnsBeforeFullStdoutGateReleaseLosslessly";
        std::string color = "--gtest_color=no";
        char *args[]{executable.data(), filter.data(), color.data(), nullptr};
        std::string marker_env = "AA_LOG_PIPE_CHILD=1";
        std::vector<char *> environment;
        for (char **entry = ::environ; *entry; ++entry) { environment.push_back(*entry); }
        environment.push_back(marker_env.data());
        environment.push_back(nullptr);
        pid_t isolated = -1;
        ASSERT_EQ(::posix_spawn(&isolated, executable.c_str(), nullptr, nullptr,
                               args, environment.data()), 0);
        int exited = static_cast<int>(::syscall(SYS_pidfd_open, isolated, 0));
        const bool normal = exited >= 0 && log_readable(exited, std::chrono::seconds{20});
        if (!normal) { (void)::kill(isolated, SIGKILL); }
        LogChild::close_fd(exited);
        int status = -1;
        pid_t waited;
        do { waited = ::waitpid(isolated, &status, 0); } while (waited < 0 && errno == EINTR);
        ASSERT_EQ(waited, isolated);
        EXPECT_TRUE(normal);
        ASSERT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 0);
        return;
    }
    ASSERT_EQ(std::distance(std::filesystem::directory_iterator{"/proc/self/task"},
                            std::filesystem::directory_iterator{}), 1);
    LogChild child;
    ASSERT_EQ(::pipe2(child.output, O_CLOEXEC), 0);
    ASSERT_EQ(::pipe2(child.control, O_CLOEXEC), 0);
    const int flags = ::fcntl(child.output[1], F_GETFL);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(::fcntl(child.output[1], F_SETFL, flags | O_NONBLOCK), 0);
    const std::string filler(4096, 'F');
    std::size_t filled = 0;
    for (;;) {
        const auto count = ::write(child.output[1], filler.data(), filler.size());
        if (count > 0) { filled += static_cast<std::size_t>(count); continue; }
        if (count < 0 && errno == EINTR) { continue; }
        ASSERT_EQ(count, -1);
        ASSERT_EQ(errno, EAGAIN);
        break;
    }
    ASSERT_GT(filled, 0U);
    ASSERT_EQ(::write(child.output[1], "F", 1), -1);
    ASSERT_EQ(errno, EAGAIN);
    ASSERT_EQ(::fcntl(child.output[1], F_SETFL, flags & ~O_NONBLOCK), 0);
    ASSERT_EQ(::fcntl(child.output[1], F_GETFL) & O_NONBLOCK, 0);
    ASSERT_EQ(std::fflush(nullptr), 0);
    const std::string json = "{\"event\":\"issue60_log_boundary\",\"sequence\":1}";
    child.pid = ::fork();
    ASSERT_GE(child.pid, 0);
    if (child.pid == 0) {
        ::close(child.output[0]);
        ::close(child.control[0]);
        if (::dup2(child.output[1], STDOUT_FILENO) < 0) { ::_exit(90); }
        ::close(child.output[1]);
        aa::wired_receiver::StdoutLogScope logging{g_running};
        if (::write(child.control[1], "R", 1) != 1) { ::_exit(91); }
        // When: actual log_line sees a held/full blocking native pipe.
        log_line(json);
        if (::write(child.control[1], "W", 1) != 1) { ::_exit(92); }
        ::close(child.control[1]);
        const auto error = logging.log.finish();
        std::exit(error ? 94 : 0);
    }
    child.exited = static_cast<int>(::syscall(SYS_pidfd_open, child.pid, 0));
    LogChild::close_fd(child.output[1]);
    LogChild::close_fd(child.control[1]);
    char marker = 0;
    const bool started = log_readable(child.control[0]) &&
        ::read(child.control[0], &marker, 1) == 1 && marker == 'R';
    const bool witness_before_release = started && log_readable(child.control[0]) &&
        ::read(child.control[0], &marker, 1) == 1 && marker == 'W';
    const int read_flags = ::fcntl(child.output[0], F_GETFL);
    child.io_ok = read_flags >= 0 &&
        ::fcntl(child.output[0], F_SETFL, read_flags | O_NONBLOCK) == 0;
    if (!child.io_ok) { (void)::kill(child.pid, SIGKILL); child.forced = true; }
    child.finish();
    const bool witness_after_release = witness_before_release ||
        (log_readable(child.control[0]) && ::read(child.control[0], &marker, 1) == 1 && marker == 'W');
    // Then: unchanged witness predicate, exact bytes, and normal owned cleanup.
    EXPECT_TRUE(started);
    EXPECT_GE(child.exited, 0);
    EXPECT_TRUE(child.io_ok);
    EXPECT_TRUE(child.eof);
    EXPECT_FALSE(child.forced);
    ASSERT_TRUE(WIFEXITED(child.status)) << child.status;
    EXPECT_EQ(WEXITSTATUS(child.status), 0);
    EXPECT_TRUE(witness_after_release);
    EXPECT_EQ(child.bytes, std::string(filled, 'F') + json + '\n');
    std::cout << "ISSUE60 filled=" << filled << " captured=" << child.bytes.size()
              << " witness_before_release=" << witness_before_release
              << " witness_after_release=" << witness_after_release
              << " eof=" << child.eof << " forced=" << child.forced
              << " child_status=" << child.status << '\n';
    EXPECT_TRUE(witness_before_release)
        << "LOG_LINE_BLOCKED_ON_FULL_STDOUT: witness required before parent drains any byte";
}

struct LogWriteProbe {
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    bool timed_out = false;
    bool interrupt = false;
    bool partial = false;
    bool zero = false;
    int error = 0;
    int calls = 0;
    std::string bytes;
    static inline LogWriteProbe *active = nullptr;
    void release() {
        const std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
    static ssize_t write(int fd, const void *data, std::size_t size) {
        EXPECT_EQ(fd, STDOUT_FILENO);
        auto &probe = *active;
        std::unique_lock lock(probe.mutex);
        ++probe.calls;
        if (!probe.cv.wait_for(lock, std::chrono::seconds{5}, [&] { return probe.released; })) {
            probe.timed_out = true;
        }
        if (probe.interrupt) { probe.interrupt = false; errno = EINTR; return -1; }
        if (probe.zero) { return 0; }
        if (probe.error) { errno = probe.error; return -1; }
        const auto count = probe.partial ? std::min(size, std::size_t{7}) : size;
        probe.bytes.append(static_cast<const char *>(data), count);
        return static_cast<ssize_t>(count);
    }
};

struct LogWriterHarness {
    LogWriteProbe probe;
    std::atomic<bool> running{true};
    aa::wired_receiver::StdoutLog log{running, LogWriteProbe::write};
    std::thread producer, follower;
    LogWriterHarness() { LogWriteProbe::active = &probe; }
    void drain() {
        probe.release();
        if (producer.joinable()) { producer.join(); }
        if (follower.joinable()) { follower.join(); }
    }
    ~LogWriterHarness() {
        drain();
        EXPECT_EQ(log.finish().value(), probe.zero ? EIO : probe.error);
        EXPECT_FALSE(probe.timed_out);
        LogWriteProbe::active = nullptr;
    }
};

void expect_stdout_capacity(bool entries) {
    // Given: the native-write seam holds the front entry, still fully charged.
    LogWriterHarness harness;
    const std::string record(entries ? 0 : aa::wired_receiver::StdoutLog::byte_capacity - 1, 'B');
    const std::size_t count = entries ? aa::wired_receiver::StdoutLog::entry_capacity : 1;
    std::atomic<bool> returned{false};
    // When: one more entry is submitted than the selected budget permits.
    harness.producer = std::thread([&] {
        for (std::size_t i = 0; i < count; ++i) { EXPECT_FALSE(harness.log.submit(record)); }
        EXPECT_FALSE(harness.log.submit("tail"));
        returned.store(true);
    });
    const bool waited = harness.log.wait_for_capacity_wait();
    const auto outstanding = harness.log.outstanding();
    const bool early_return = returned.load();
    harness.drain();
    // Then: capacity backpressures without evicting the front or any tail byte.
    EXPECT_TRUE(waited);
    EXPECT_FALSE(early_return);
    EXPECT_EQ(outstanding.first, entries ? count : aa::wired_receiver::StdoutLog::byte_capacity);
    EXPECT_EQ(outstanding.second, count);
    EXPECT_FALSE(harness.log.finish());
    const auto expected = entries ? std::string(count, '\n') : record + '\n';
    EXPECT_EQ(harness.probe.bytes, expected + "tail\n");
    EXPECT_FALSE(harness.log.finish());
    EXPECT_EQ(harness.log.submit("closed").value(), ESHUTDOWN);
}
TEST(WiredReceiver, StdoutLogByteCapacityIncludesBlockedFront) { expect_stdout_capacity(false); }
TEST(WiredReceiver, StdoutLogEntryCapacityIncludesBlockedFront) { expect_stdout_capacity(true); }

TEST(WiredReceiver, StdoutLogOversizeRemainsContiguousAcrossCapacityWait) {
    // Given: an oversized record with a competing producer behind it.
    LogWriterHarness harness;
    const std::string record(aa::wired_receiver::StdoutLog::byte_capacity + 17, 'X');
    harness.producer = std::thread([&] { EXPECT_FALSE(harness.log.submit(record)); });
    const bool waited = harness.log.wait_for_capacity_wait();
    const auto outstanding = harness.log.outstanding();
    // When: a second producer contends while the oversized call holds admission.
    harness.follower = std::thread([&] { EXPECT_FALSE(harness.log.submit("tail")); });
    harness.drain();
    // Then: chunks fit the budget and reconstruct one uninterrupted record.
    EXPECT_TRUE(waited);
    EXPECT_EQ(outstanding, (std::pair<std::size_t, std::size_t>{1'048'576, 1}));
    EXPECT_FALSE(harness.log.finish());
    EXPECT_EQ(harness.probe.bytes, record + "\ntail\n");
}

TEST(WiredReceiver, StdoutLogFlushHandlesPartialWritesAndEintrBeforeFinish) {
    // Given: a writer that interrupts once and then accepts only seven bytes.
    LogWriterHarness harness;
    harness.probe.interrupt = true;
    harness.probe.partial = true;
    EXPECT_FALSE(harness.log.submit("{\"timestamp_ns\":123456789,\"sequence\":1}"));
    EXPECT_FALSE(harness.log.submit("second"));
    // When: a flush barrier waits behind the charged front, then it is released.
    std::atomic<bool> returned{false};
    harness.follower = std::thread([&] {
        EXPECT_FALSE(harness.log.flush());
        returned.store(true);
    });
    const bool waited = harness.log.wait_for_barrier(false);
    const bool early_return = returned.load();
    harness.drain();
    // Then: the barrier observes exact FIFO bytes, not just an emptied staging queue.
    EXPECT_TRUE(waited);
    EXPECT_FALSE(early_return);
    EXPECT_EQ(harness.probe.bytes, "{\"timestamp_ns\":123456789,\"sequence\":1}\nsecond\n");
    EXPECT_FALSE(harness.log.finish());
    EXPECT_FALSE(harness.log.finish());
}

TEST(WiredReceiver, StdoutLogFinishWaitsForFrontAndJoinsOnlyOnce) {
    // Given: an accepted record still held inside the native write seam.
    LogWriterHarness harness;
    EXPECT_FALSE(harness.log.submit("final"));
    std::atomic<bool> returned{false};
    // When: finish closes admission while the front is still outstanding.
    harness.follower = std::thread([&] {
        EXPECT_FALSE(harness.log.finish());
        returned.store(true);
    });
    const bool waited = harness.log.wait_for_barrier(true);
    const bool early_return = returned.load();
    harness.drain();
    // Then: normal finish waits for exact delivery; repeat finish is harmless.
    EXPECT_TRUE(waited);
    EXPECT_FALSE(early_return);
    EXPECT_EQ(harness.probe.bytes, "final\n");
    EXPECT_FALSE(harness.log.finish());
    EXPECT_EQ(harness.log.submit("closed").value(), ESHUTDOWN);
}

TEST(WiredReceiver, StdoutLogRequiresExplicitBinding) {
    // Given: a fresh process without an active writer binding.
    const auto previous = testing::FLAGS_gtest_death_test_style;
    testing::FLAGS_gtest_death_test_style = "threadsafe";
    // When: a callback is used outside the receiver resource lifetime.
    // Then: this is a lifecycle bug, never a synchronous stdout fallback.
    EXPECT_DEATH(log_line("unbound"), "");
    testing::FLAGS_gtest_death_test_style = previous;
}

TEST(WiredReceiver, StdoutLogTerminalErrorsWakeFullProducerAndFailEveryBarrier) {
    // Given: a full queue whose front will fail instead of completing.
    for (const int error : {EAGAIN, EPIPE, EBADF, EIO}) {
        testing::internal::CaptureStderr();
        LogWriterHarness harness;
        harness.probe.error = error;
        harness.probe.zero = error == EIO;
        const std::string record(aa::wired_receiver::StdoutLog::byte_capacity - 1, 'E');
        EXPECT_FALSE(harness.log.submit(record));
        std::error_code submission;
        harness.producer = std::thread([&] { submission = harness.log.submit("waiting"); });
        const bool waited = harness.log.wait_for_capacity_wait();
        // When: the writer reports a terminal syscall error (or zero progress).
        harness.drain();
        // Then: no false delivery/clean finish, no retry/fallback, no stuck waiter.
        EXPECT_TRUE(waited);
        EXPECT_EQ(submission.value(), error);
        EXPECT_FALSE(harness.running.load());
        EXPECT_EQ(harness.log.flush().value(), error);
        EXPECT_EQ(harness.log.finish().value(), error);
        EXPECT_EQ(harness.log.finish().value(), error);
        EXPECT_EQ(harness.log.submit("late").value(), error);
        EXPECT_TRUE(harness.probe.bytes.empty());
        EXPECT_EQ(harness.probe.calls, 1);
        EXPECT_EQ(testing::internal::GetCapturedStderr(),
                  "stdout_log errno=" + std::to_string(error) + '\n');
    }
}

TEST(WiredReceiver, StdoutLogOwnsViewsAndCaptureBarriersSeparateFreshScopes) {
    // Given: transient serialized inputs, repeated fresh bindings and captures.
    for (int round = 0; round < 2; ++round) {
        ReceiverLogScope logging{g_running};
        capture_receiver_stdout();
        std::string expected;
        // When: actual log_line accepts about 100 KB, then source buffers change.
        for (int i = 0; i < 100; ++i) {
            std::string record = "{\"timestamp_ns\":123456789,\"sequence\":" +
                std::to_string(i) + ",\"payload\":\"" + std::string(1000, 'V') + "\"}";
            expected += record + '\n';
            log_line(record);
            record.assign(record.size(), 'Z');
        }
        // Then: the real stdout barrier preserves every original byte and timestamp.
        EXPECT_EQ(captured_receiver_stdout(), expected);
        EXPECT_FALSE(logging.log.finish());
    }
    EXPECT_EQ(aa::wired_receiver::active_stdout_log.load(), nullptr);
}

struct StdoutRestore {
    int saved = ::dup(STDOUT_FILENO);
    void restore() {
        if (saved >= 0) {
            (void)::dup2(saved, STDOUT_FILENO);
            LogChild::close_fd(saved);
        }
    }
    ~StdoutRestore() { restore(); }
};

TEST(WiredReceiver, StdoutLogBrokenPipeFailsWithoutChangingCallerSigpipeMask) {
    // Given: a real pipe with no reader and an unchanged caller signal mask.
    LogChild pipe;
    ASSERT_EQ(::pipe2(pipe.output, O_CLOEXEC), 0);
    LogChild::close_fd(pipe.output[0]);
    sigset_t before, after;
    ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, nullptr, &before), 0);
    struct sigaction disposition_before{}, disposition_after{};
    ASSERT_EQ(::sigaction(SIGPIPE, nullptr, &disposition_before), 0);
    StdoutRestore restore;
    ASSERT_GE(restore.saved, 0);
    ASSERT_EQ(::dup2(pipe.output[1], STDOUT_FILENO), STDOUT_FILENO);
    std::atomic<bool> running{true};
    aa::wired_receiver::StdoutLog log{running};
    // When: only the writer thread attempts the broken native stdout write.
    EXPECT_FALSE(log.submit("broken"));
    const auto error = log.finish();
    restore.restore();
    // Then: the failure is EPIPE, not process death or a process-wide signal change.
    EXPECT_EQ(error.value(), EPIPE);
    EXPECT_EQ(log.flush(), error);
    EXPECT_FALSE(running.load());
    ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, nullptr, &after), 0);
    ASSERT_EQ(::sigaction(SIGPIPE, nullptr, &disposition_after), 0);
    EXPECT_EQ(::sigismember(&before, SIGPIPE), ::sigismember(&after, SIGPIPE));
    EXPECT_EQ(disposition_before.sa_handler, disposition_after.sa_handler);
}

TEST(WiredReceiver, VideoOutletNoConsumerDropsAreNotSent) {
    ReceiverLogScope logging{g_running};
    VideoOutlet outlet{log_line};
    const std::vector<std::byte> payload{std::byte{0}, std::byte{0}, std::byte{1},
                                         std::byte{0x65}, std::byte{0x10}};
    for (int index = 0; index < 31; ++index) {
        capture_receiver_stdout();
        const auto result = outlet.on_message({{aa::protocol::ChannelRole::video, 1, {}}, payload});
        const auto log = captured_receiver_stdout();
        ASSERT_TRUE(result);
        if (index % 30 == 0) {
            EXPECT_EQ(log, "{\"event\":\"video_dropped\",\"reason\":\"no_consumer\",\"count\":" +
                               std::to_string(index + 1) + "}\n" +
                               "{\"event\":\"decode_stats\",\"frame_drops\":" +
                               std::to_string(index + 1) + "}\n");
        } else {
            EXPECT_TRUE(log.empty());
        }
    }
}

TEST(WiredReceiver, VideoOutletWaitingIdrDropsDoNotCountAsSent) {
    ReceiverLogScope logging{g_running};
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    VideoOutlet outlet{log_line};
    outlet.install(std::move(pair.value().producer));
    const std::vector<std::byte> non_idr{std::byte{0}, std::byte{0}, std::byte{1},
                                         std::byte{0x41}, std::byte{0x10}};
    const std::vector<std::byte> idr{std::byte{0}, std::byte{0}, std::byte{1},
                                     std::byte{0x65}, std::byte{0x10}};
    for (int index = 0; index < 31; ++index) {
        capture_receiver_stdout();
        const auto result = outlet.on_message({{aa::protocol::ChannelRole::video, 1, {}}, non_idr});
        const auto log = captured_receiver_stdout();
        ASSERT_TRUE(result);
        if (index % 30 == 0) {
            EXPECT_EQ(log, "{\"event\":\"video_dropped\",\"reason\":\"waiting_idr\",\"count\":" +
                               std::to_string(index + 1) + "}\n" +
                               "{\"event\":\"decode_stats\",\"frame_drops\":" +
                               std::to_string(index + 1) + "}\n");
        } else {
            EXPECT_TRUE(log.empty());
        }
    }
    const auto empty = pair.value().consumer.receive(Milliseconds{0});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code(), aa::ErrorCode::timeout);

    for (std::uint64_t delivered = 1; delivered <= 31; ++delivered) {
        const auto &payload = delivered == 1 ? idr : non_idr;
        capture_receiver_stdout();
        const auto result = outlet.on_message({{aa::protocol::ChannelRole::video, 1, {123}}, payload});
        const auto log = captured_receiver_stdout();
        ASSERT_TRUE(result);
        ASSERT_TRUE(log.starts_with("{\"event\":\"video_ipc_send\","));
        const auto endpoint_end = log.find('\n');
        ASSERT_NE(endpoint_end, std::string::npos);
        const auto summary = log.substr(endpoint_end + 1);
        if (delivered % 30 == 1) {
            EXPECT_EQ(summary, "{\"event\":\"video_sent\",\"units\":" +
                               std::to_string(delivered) + "}\n" +
                               "{\"event\":\"decode_stats\",\"frame_drops\":31}\n");
        } else {
            EXPECT_TRUE(summary.empty());
        }
        const auto received = pair.value().consumer.receive(Milliseconds{500});
        ASSERT_TRUE(received);
        ASSERT_EQ(received.value(), aa::ipc::ReceiveStatus::complete);
        const auto &unit = pair.value().consumer.access_unit();
        EXPECT_EQ(unit.frame, 31 + delivered);
        EXPECT_EQ(unit.timestamp_ns, 123U);
        EXPECT_EQ(unit.idr, delivered == 1);
        EXPECT_EQ(unit.payload, (std::vector<std::uint8_t>{0, 0, 1,
                                  static_cast<std::uint8_t>(delivered == 1 ? 0x65 : 0x41), 0x10}));
    }
}

TEST(WiredReceiver, VideoOutletSendErrorsDoNotCountAsSent) {
    ReceiverLogScope logging{g_running};
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    VideoOutlet outlet{log_line};
    outlet.install(std::move(pair.value().producer));
    capture_receiver_stdout();
    const auto invalid = outlet.on_message({{aa::protocol::ChannelRole::video, 1, {}}, {}});
    const auto invalid_log = captured_receiver_stdout();
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code(), aa::ErrorCode::invalid_argument);
    EXPECT_TRUE(invalid_log.empty());

    const std::vector<std::byte> idr{std::byte{0}, std::byte{0}, std::byte{1},
                                     std::byte{0x65}, std::byte{0x10}};
    capture_receiver_stdout();
    const auto sent = outlet.on_message({{aa::protocol::ChannelRole::video, 1, {}}, idr});
    const auto sent_log = captured_receiver_stdout();
    ASSERT_TRUE(sent);
    ASSERT_TRUE(sent_log.starts_with("{\"event\":\"video_ipc_send\","));
    const auto endpoint_end = sent_log.find('\n');
    ASSERT_NE(endpoint_end, std::string::npos);
    EXPECT_EQ(sent_log.substr(endpoint_end + 1), "{\"event\":\"video_sent\",\"units\":1}\n"
                                               "{\"event\":\"decode_stats\",\"frame_drops\":0}\n");
    const auto received = pair.value().consumer.receive(Milliseconds{500});
    ASSERT_TRUE(received);
    ASSERT_EQ(received.value(), aa::ipc::ReceiveStatus::complete);
    EXPECT_EQ(pair.value().consumer.access_unit().frame, 2U);
}

struct ReceiverAudioExpectation {
    std::int32_t service;
    aa::protocol::ChannelRole channel;
    aa::audio::Role role;
    int wire_role;
    aa::audio::AudioFormat input;
};

const std::array receiver_audio_expectations{
    ReceiverAudioExpectation{100, aa::protocol::ChannelRole::media_audio, aa::audio::Role::media,
                             media_wire::AUDIO_STREAM_MEDIA, {48000, 2, 16}},
    ReceiverAudioExpectation{101, aa::protocol::ChannelRole::guidance_audio, aa::audio::Role::guidance,
                             media_wire::AUDIO_STREAM_GUIDANCE, {16000, 1, 16}},
    ReceiverAudioExpectation{102, aa::protocol::ChannelRole::system_audio, aa::audio::Role::system,
                             media_wire::AUDIO_STREAM_SYSTEM_AUDIO, {16000, 1, 16}},
    ReceiverAudioExpectation{103, aa::protocol::ChannelRole::speech_audio, aa::audio::Role::speech,
                             media_wire::AUDIO_STREAM_TELEPHONY, {16000, 1, 16}},
};

TEST(WiredReceiver, ProductionAudioProfilesMatchWireAndRouterInputs) {
    const auto services = service_descriptors(aa::input::CapabilityDescriptor::bench_defaults());
    const auto encoded = aa::protocol::encode(aa::protocol::ServiceDiscoveryResponse{services});
    ASSERT_TRUE(encoded.has_value());
    ASSERT_GE(encoded.value().size(), 2U);
    aap_protobuf::service::control::message::ServiceDiscoveryResponse wire;
    ASSERT_TRUE(wire.ParseFromArray(encoded.value().data() + 2,
                                   static_cast<int>(encoded.value().size() - 2)));
    const auto decoded = aa::protocol::decode_service_discovery_response(encoded.value());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value().services, services);
    ASSERT_EQ(wire.channels_size(), 7);
    const auto router = pcm_router_config();
    EXPECT_EQ(router.output_format, (aa::audio::AudioFormat{48000, 2, 16}));
    EXPECT_TRUE(router.on_write_completed);
    for (const auto &expected : receiver_audio_expectations) {
        if (expected.channel == aa::protocol::ChannelRole::speech_audio) { continue; }
        SCOPED_TRACE(expected.service);
        const auto entry = std::find_if(wire.channels().begin(), wire.channels().end(),
            [&](const auto &channel) { return channel.id() == expected.service; });
        ASSERT_NE(entry, wire.channels().end());
        ASSERT_TRUE(entry->has_media_sink_service());
        const auto &media = entry->media_sink_service();
        EXPECT_EQ(media.audio_type(), expected.wire_role);
        EXPECT_EQ(media.available_type(),
                  aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
        ASSERT_EQ(media.audio_configs_size(), 1);
        const auto &profile = media.audio_configs(0);
        EXPECT_EQ(profile.sampling_rate(), expected.input.sample_rate_hz);
        EXPECT_EQ(profile.number_of_bits(), expected.input.bits_per_sample);
        EXPECT_EQ(profile.number_of_channels(), expected.input.channels);
        const auto &input = router.roles[aa::audio::role_index(expected.role)];
        EXPECT_EQ(input.codec, aa::audio::Codec::pcm);
        EXPECT_EQ(input.input_format, expected.input);
        EXPECT_EQ(input.input_format.sample_rate_hz, profile.sampling_rate());
        EXPECT_EQ(input.input_format.bits_per_sample, profile.number_of_bits());
        EXPECT_EQ(input.input_format.channels, profile.number_of_channels());
        const auto descriptor = std::find_if(decoded.value().services.begin(),
            decoded.value().services.end(),
            [&](const auto &service) { return service.id.value == expected.service; });
        ASSERT_NE(descriptor, decoded.value().services.end());
        EXPECT_EQ(descriptor->role, expected.channel);
    }
    const auto mic = std::find_if(wire.channels().begin(), wire.channels().end(),
        [](const auto &entry) { return entry.id() == 106; });
    ASSERT_NE(mic, wire.channels().end());
    ASSERT_TRUE(mic->has_media_source_service());
    const auto &source = mic->media_source_service();
    EXPECT_EQ(source.available_type(),
              aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
    EXPECT_TRUE(source.has_available_while_in_call());
    EXPECT_FALSE(source.available_while_in_call());
    ASSERT_TRUE(source.has_audio_config());
    EXPECT_EQ(source.audio_config().sampling_rate(), 16000u);
    EXPECT_EQ(source.audio_config().number_of_bits(), 16u);
    EXPECT_EQ(source.audio_config().number_of_channels(), 1u);
    for (const auto &entry : wire.channels()) { EXPECT_NE(entry.id(), 103); }
}

class ReceiverProfileSink final : public aa::audio::detail::ObservedAudioSink {
  public:
    aa::audio::AudioFormat format{};
    std::size_t writes{};
    std::vector<std::byte> pcm;
    aa::core::Result<void> write(const aa::audio::MixedChunk &chunk) override {
        ++writes;
        format = chunk.format;
        pcm.insert(pcm.end(), chunk.pcm.begin(), chunk.pcm.end());
        return {};
    }
    aa::core::Result<void> drain(aa::core::Milliseconds) override { return {}; }
    aa::audio::SinkProbeStats probe_stats() const override { return {}; }
    aa::audio::RenderBarrierStats render_stats() const override { return {}; }
};

TEST(WiredReceiver, ProductionAvRegistrationNegotiatesEncryptedMediaWithoutSetupPcm) {
    ReceiverLogScope logging{g_running};
    namespace p = aa::protocol;
    namespace tr = aa::transport;
    aa::session::test::MapTrustStore trust;
    aa::session::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::ScriptedTransport transport;
    const ReceiverProfileSink *observed = nullptr;
    ConcreteAudioRenderWorker audio{pcm_router_config(), [&](const auto &config) {
        auto output = std::make_unique<ReceiverProfileSink>();
        observed = output.get();
        return aa::audio::detail::open_router_with_sink_for_test(config, std::move(output));
    }};
    ASSERT_TRUE(audio.ready());
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    VideoOutlet video{log_line};
    video.install(std::move(pair.value().producer));
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    ASSERT_TRUE(session.configure(service_descriptors(aa::input::CapabilityDescriptor::bench_defaults())));
    ASSERT_TRUE(register_av_sinks(session, video, audio));
    session.attach_transport(&transport);
    auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
    auto *cipher = owned.get();
    session.attach_cipher(std::move(owned));
    wiredtest::walk_auth(session, transport);
    ASSERT_TRUE(session.authenticated());
    ASSERT_TRUE(session.announce_services());
    const auto receive = [&](std::uint8_t channel, std::vector<std::byte> payload) {
        auto frames = tr::encode_message({channel, tr::MessageKind::specific, std::move(payload)},
                                         tr::Encryption::encrypted, cipher);
        if (!frames) { return aa::core::Result<bool>{frames.error()}; }
        for (auto &frame : frames.value()) { transport.push_inbound(std::move(frame)); }
        return session.poll_transport();
    };
    const auto reply = [&](std::uint8_t channel, const std::vector<std::byte> &expected) {
        const auto frame = tr::decode_frame(transport.last_sent());
        ASSERT_TRUE(frame);
        EXPECT_EQ(frame.value().header.channel, channel);
        EXPECT_EQ(frame.value().header.kind, tr::MessageKind::specific);
        EXPECT_EQ(frame.value().header.encryption, tr::Encryption::encrypted);
        const auto payload = cipher->open(frame.value().payload);
        ASSERT_TRUE(payload);
        EXPECT_EQ(payload.value(), expected);
    };
    for (const std::uint8_t channel : {std::uint8_t{100}, std::uint8_t{101}, std::uint8_t{102}}) {
        const auto before = transport.sent_count();
        ASSERT_TRUE(receive(channel, {std::byte{0x80}, std::byte{0}, std::byte{8}, std::byte{1}}));
        EXPECT_EQ(transport.sent_count(), before + 1);
        EXPECT_TRUE(observed->pcm.empty()) << "Setup reached the production PCM router";
        EXPECT_EQ(observed->writes, 0U);
        reply(channel, p::encode_av_sink_ready().value());
    }
    ASSERT_TRUE(receive(99, p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    reply(99, p::encode(p::VideoFocusNotification{p::VideoFocusMode::projected, true}).value());
    ASSERT_TRUE(receive(99, p::encode(p::AvSinkStart{-7, 0}).value()));
    auto config = std::vector<std::byte>{std::byte{0}, std::byte{1}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x67}, std::byte{0x42}};
    ASSERT_TRUE(receive(99, config));
    reply(99, p::encode_av_sink_ack(-7).value());
    const auto empty = pair.value().consumer.receive(Milliseconds{0});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code(), aa::ErrorCode::timeout);
    std::vector<std::byte> data(10, std::byte{0x55});
    data[0] = data[1] = std::byte{0};
    data.insert(data.end(), wiredtest::kVideoIdr.begin(), wiredtest::kVideoIdr.end());
    clock.set(aa::core::Nanoseconds{987654});
    ASSERT_TRUE(receive(99, data));
    reply(99, p::encode_av_sink_ack(-7).value());
    ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
    const auto &unit = pair.value().consumer.access_unit();
    EXPECT_EQ(unit.timestamp_ns, 987654U);
    std::vector<std::uint8_t> expected;
    for (auto it = config.begin() + 2; it != config.end(); ++it) {
        expected.push_back(std::to_integer<std::uint8_t>(*it));
    }
    for (const auto byte : wiredtest::kVideoIdr) { expected.push_back(std::to_integer<std::uint8_t>(byte)); }
    EXPECT_EQ(unit.payload, expected);
    EXPECT_TRUE(unit.idr);
    ASSERT_TRUE(receive(100, p::encode(p::AvSinkStart{0, 0}).value()));
    data.resize(10);
    const auto pcm = wiredtest::pcm_unit();
    data.insert(data.end(), pcm.begin(), pcm.end());
    ASSERT_TRUE(receive(100, data));
    const auto completion = audio.take_completion(std::chrono::steady_clock::now() +
                                                 std::chrono::seconds{2});
    ASSERT_TRUE(completion);
    ASSERT_TRUE(session.complete_av_delivery(*completion, {}));
    reply(100, p::encode_av_sink_ack(0).value());
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    EXPECT_EQ(observed->pcm, pcm);
    EXPECT_EQ(observed->writes, 1U);
    ASSERT_TRUE(receive(0, p::encode(p::PingRequest{42, {}}).value()));
    EXPECT_EQ(session.pings_answered(), 1U);
}

TEST(WiredReceiver, ProductionAudioInputsRouteTo48kStereoAndKeepCompletionCallback) {
    ReceiverLogScope logging{g_running};
    for (const auto &expected : receiver_audio_expectations) {
        SCOPED_TRACE(expected.service);
        std::mutex completion_mutex;
        std::condition_variable completion_cv;
        bool completion_published = false;
        auto config = pcm_router_config();
        config.on_write_completed = [&, publish = config.on_write_completed](const auto &value) {
            publish(value);
            const std::lock_guard lock(completion_mutex);
            completion_published = true;
            completion_cv.notify_all();
        };
        const ReceiverProfileSink *observed = nullptr;
        ConcreteAudioRenderWorker sink{config, [&](const auto &router_config) {
            auto output = std::make_unique<ReceiverProfileSink>();
            observed = output.get();
            return aa::audio::detail::open_router_with_sink_for_test(router_config, std::move(output));
        }};
        ASSERT_TRUE(sink.ready());
        const std::size_t frames = expected.input.sample_rate_hz / 50U;
        const std::vector<std::byte> pcm(frames * expected.input.channels * sizeof(std::int16_t),
                                         std::byte{0x10});
        capture_receiver_stdout();
        const auto result = sink.submit({{expected.channel, 1, {}}, pcm},
                                         {expected.channel, 0, 1, 1});
        if (expected.channel == aa::protocol::ChannelRole::speech_audio) {
            (void)captured_receiver_stdout();
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code(), aa::ErrorCode::invalid_argument);
            EXPECT_FALSE(sink.outstanding());
            continue;
        }
        EXPECT_TRUE(sink.take_completion(std::chrono::steady_clock::now() +
                                         std::chrono::seconds{2}));
        ASSERT_TRUE(sink.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        bool published;
        {
            std::unique_lock lock(completion_mutex);
            published = completion_cv.wait_for(lock, std::chrono::seconds{2}, [&] { return completion_published; });
        }
        const auto log = captured_receiver_stdout();
        ASSERT_TRUE(published);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(observed->format, (aa::audio::AudioFormat{48000, 2, 16}));
        // Resampling may retain a filter tail; require the 3x rate expansion,
        // allowing that bounded tail rather than assuming one exact chunk size.
        if (expected.input.channels == 1) {
            EXPECT_GT(observed->pcm.size(), frames * 2U * 2U * sizeof(std::int16_t));
            EXPECT_LE(observed->pcm.size(), frames * 3U * 2U * sizeof(std::int16_t));
        } else {
            EXPECT_EQ(observed->pcm.size(), pcm.size());
        }
        ASSERT_FALSE(observed->pcm.empty());
        ASSERT_EQ(observed->pcm.size() % 4U, 0U);
        EXPECT_TRUE(std::any_of(observed->pcm.begin(), observed->pcm.end(),
                               [](std::byte value) { return value != std::byte{0}; }));
        for (std::size_t offset = 0; offset < observed->pcm.size(); offset += 4U) {
            EXPECT_EQ(observed->pcm[offset], observed->pcm[offset + 2U]);
            EXPECT_EQ(observed->pcm[offset + 1U], observed->pcm[offset + 3U]);
        }
        EXPECT_NE(log.find("\"event\":\"audio_write_completed\""), std::string::npos);
        EXPECT_NE(log.find("\"scheduled_end_ns\":"), std::string::npos);
    }
}

std::atomic<int> runtime_connects{0};
std::atomic<int> runtime_ciphers{0};
std::atomic<int> runtime_sends{0};
bool runtime_recovers = false;
bool runtime_quiet = false;
std::atomic<bool> runtime_waiting{false};
std::atomic<bool> runtime_closed{false};
std::mutex runtime_owner_mutex;
std::thread::id runtime_io_owner;
aa::trust::PhoneIdentity runtime_phone;
bool runtime_real_admission = false;
bool runtime_fail_before_admission = false;
bool runtime_send_audio = false;
std::size_t runtime_audio_bytes = 0;
bool runtime_ack_send_failure = false;
std::atomic<int> runtime_audio_ack_attempts{0};
std::atomic<int> runtime_polls{0};
std::function<void()> runtime_on_poll;
std::function<std::unique_ptr<aa::audio::detail::ObservedAudioSink>()> runtime_sink_override;
std::function<aa::audio::detail::SinkOpenOutcome(const aa::audio::detail::SinkOpenRequest &)>
    runtime_sink_request_override;
std::shared_ptr<aa::test::usb_aoa::LinkState> admission_state;

// Absolute production deadlines, not an unlimited source driven by admission ACKs.
// Owner-thread-only state; the sink has its own synchronized RenderGate below.
struct CadencedPcmSource {
    using Clock = std::chrono::steady_clock;
    bool burst = false;
    std::size_t chunks = 8;
    Clock::time_point epoch{};
    std::vector<std::int64_t> arrivals;
    std::vector<std::int64_t> acks;
    std::vector<std::size_t> held_chunks; // Due chunk indices held for credit.
    std::int64_t elapsed_ns() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - epoch).count();
    }
    Clock::time_point due() const {
        return epoch + std::chrono::nanoseconds{burst ? 0 :
            static_cast<std::int64_t>(arrivals.size()) * 8192 * 1'000'000'000LL / 192000};
    }
};
CadencedPcmSource *runtime_pcm_source = nullptr;

class RuntimeTransport final : public aa::transport::Transport {
  public:
    explicit RuntimeTransport(bool drops) : drops_(drops) {
        frames_.push_back(wiredtest::control_frame(
            aa::protocol::encode(aa::protocol::VersionResponse{1, 6, 0}).value()));
        frames_.push_back(wiredtest::control_frame(
            aa::protocol::encode(aa::protocol::EncapsulatedSsl{{std::byte{1}}}).value()));
        if (runtime_send_audio) { frames_.push_back(wiredtest::discovery_request_frame()); }
        frames_.push_back(wiredtest::open_request_frame(99));
        if (runtime_send_audio) {
            wiredtest::FakeHandshakeCipher cipher;
            (void)cipher.step_handshake();
            (void)cipher.step_handshake();
            std::vector<std::byte> data(10, std::byte{0});
            const auto pcm = runtime_audio_bytes == 0 ? wiredtest::pcm_unit()
                : std::vector<std::byte>(runtime_audio_bytes, std::byte{1});
            data.insert(data.end(), pcm.begin(), pcm.end());
            for (auto payload : {aa::protocol::encode(aa::protocol::AvSinkSetup{
                                      aa::protocol::AvSinkCodec::pcm}).value(),
                                  aa::protocol::encode(aa::protocol::AvSinkStart{-17, 0}).value(), data}) {
                if (runtime_pcm_source && payload == data) { continue; }
                auto encoded = aa::transport::encode_message(
                    {100, aa::transport::MessageKind::specific, std::move(payload)},
                    aa::transport::Encryption::encrypted, &cipher);
                for (auto &frame : encoded.value()) { frames_.push_back(std::move(frame)); }
            }
        }
    }
    aa::transport::Kind kind() const noexcept override { return aa::transport::Kind::usb_aoa; }
    aa::core::Result<void> open(aa::core::CancellationToken token) override {
        if (shutdown_observation) {
            shutdown_observation->io();
        }
        token_ = token;
        return {};
    }
    aa::core::Result<void> send(std::span<const std::byte> bytes) override {
        ++runtime_sends;
        if (runtime_send_audio) {
            const auto frame = aa::transport::decode_frame(bytes);
            if (frame && frame.value().header.channel == 100 &&
                frame.value().header.encryption == aa::transport::Encryption::encrypted) {
                std::vector<std::byte> plain;
                for (auto byte : frame.value().payload) {
                    plain.push_back(static_cast<std::byte>(~std::to_integer<unsigned char>(byte)));
                }
                if (plain == aa::protocol::encode_av_sink_ack(-17).value()) {
                    ++runtime_audio_ack_attempts;
                    if (runtime_pcm_source) {
                        runtime_pcm_source->acks.push_back(runtime_pcm_source->elapsed_ns());
                    }
                    if (runtime_ack_send_failure) { return aa::Error{aa::ErrorCode::transport_io}; }
                }
            }
        }
        if (shutdown_observation) {
            shutdown_observation->io();
            // Pings use plain framing. Match the complete valid response, not
            // merely an arbitrary send during startup.
            const auto reply = wiredtest::control_frame(
                aa::protocol::encode(aa::protocol::PingResponse{1, {}}).value());
            if (std::equal(bytes.begin(), bytes.end(), reply.begin(), reply.end())) {
                const std::lock_guard lock(shutdown_observation->state);
                ++shutdown_observation->replies;
            }
        }
        return {};
    }
    void set_io_checkpoint(std::function<void()> checkpoint) override {
        checkpoint_ = std::move(checkpoint);
    }
    aa::core::Result<std::vector<std::byte>> receive() override {
        if (shutdown_observation) {
            shutdown_observation->io();
        }
        {
            const std::lock_guard lock(runtime_owner_mutex);
            runtime_io_owner = std::this_thread::get_id();
        }
        if (checkpoint_) {
            checkpoint_();
        }
        if (next_ < frames_.size()) {
            return frames_[next_++];
        }
        if (shutdown_observation) {
            std::unique_lock lock(shutdown_observation->state);
            if (shutdown_observation->pings == 3) {
                shutdown_observation->receive_held = true;
                shutdown_observation->record("receive-held");
                (void)shutdown_observation->wait(lock, [&] {
                    return shutdown_observation->release_receive;
                });
                ++shutdown_observation->final_receives;
                shutdown_observation->record("final-receive");
            }
            ++shutdown_observation->pings;
            return wiredtest::control_frame(
                aa::protocol::encode(aa::protocol::PingRequest{1, {}}).value());
        }
        if (drops_ && !runtime_quiet) {
            return aa::Error{aa::ErrorCode::transport_closed};
        }
        while (runtime_pcm_source && runtime_pcm_source->arrivals.size() < runtime_pcm_source->chunks) {
            if (checkpoint_) { checkpoint_(); }
            auto polled = poll_receive(std::chrono::steady_clock::now() + std::chrono::milliseconds{5});
            if (!polled) { return polled.error(); }
            if (polled.value()) { return std::move(*polled.value()); }
        }
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        runtime_waiting.store(true);
        while (!token_.stop_requested()) {
            lock.unlock();
            if (checkpoint_) {
                checkpoint_();
            }
            lock.lock();
            cv.wait_for(lock, token_, std::chrono::milliseconds{20}, [] { return false; });
        }
        close();
        return aa::Error{aa::ErrorCode::cancelled};
    }
    aa::core::Result<std::optional<std::vector<std::byte>>>
    poll_receive(aa::core::Deadline::Clock::time_point until) override {
        ++runtime_polls;
        if (runtime_on_poll) { runtime_on_poll(); }
        if (next_ < frames_.size() || (drops_ && !runtime_quiet)) {
            auto frame = receive();
            if (!frame) { return frame.error(); }
            return std::optional{std::move(frame).value()};
        }
        if (runtime_pcm_source && runtime_pcm_source->arrivals.size() < runtime_pcm_source->chunks) {
            auto &source = *runtime_pcm_source;
            if (source.epoch == CadencedPcmSource::Clock::time_point{}) {
                source.epoch = CadencedPcmSource::Clock::now();
            }
            std::mutex mutex;
            std::condition_variable_any cv;
            std::unique_lock lock(mutex);
            const bool credited = source.acks.size() == source.arrivals.size();
            cv.wait_until(lock, token_, credited ? std::min(until, source.due()) : until,
                          [] { return false; });
            if (token_.stop_requested()) { return aa::Error{aa::ErrorCode::cancelled}; }
            if (CadencedPcmSource::Clock::now() < source.due()) {
                return std::optional<std::vector<std::byte>>{};
            }
            // A conforming max_unacked=1 phone holds a due chunk until credited.
            if (!credited) {
                if (source.held_chunks.empty() || source.held_chunks.back() != source.arrivals.size()) {
                    source.held_chunks.push_back(source.arrivals.size());
                }
                return std::optional<std::vector<std::byte>>{};
            }
            std::vector<std::byte> data(10 + 8192, std::byte{0});
            wiredtest::FakeHandshakeCipher cipher;
            (void)cipher.step_handshake();
            (void)cipher.step_handshake();
            auto frames = aa::transport::encode_message(
                {100, aa::transport::MessageKind::specific, std::move(data)},
                aa::transport::Encryption::encrypted, &cipher);
            if (!frames) { return frames.error(); }
            if (frames.value().size() != 1) { return aa::Error{aa::ErrorCode::invalid_argument}; }
            source.arrivals.push_back(source.elapsed_ns());
            return std::optional{std::move(frames.value().front())};
        }
        if (checkpoint_) { checkpoint_(); }
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait_until(lock, token_, until, [] { return false; });
        if (token_.stop_requested()) { return aa::Error{aa::ErrorCode::cancelled}; }
        return std::optional<std::vector<std::byte>>{};
    }
    void close() noexcept override {
        if (shutdown_observation) {
            shutdown_observation->cleanup_close();
        }
        runtime_closed.store(true);
    }
  private:
    bool drops_;
    std::size_t next_{};
    aa::core::CancellationToken token_;
    std::function<void()> checkpoint_;
    std::vector<std::vector<std::byte>> frames_;
};

// Phone-side double for the required-video-consumer admission regressions: a
// test-pushed frame queue the owner may only consume once the admission gate
// has opened, plus an exact record of every upstream frame the receiver sent.
// An exhausted queue blocks (like the quiet runtime link), never reports
// transport_closed, so a gated run cannot enter the reconnect loop and mask
// an ordering failure.
struct AdmissionState final {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::vector<std::vector<std::byte>> inbound;
    std::vector<std::vector<std::byte>> sent;
    std::size_t served = 0;
    bool created = false;
    bool closed = false;
};

class AdmissionTransport final : public aa::transport::Transport {
  public:
    explicit AdmissionTransport(std::shared_ptr<AdmissionState> state) : state_(std::move(state)) {}
    aa::transport::Kind kind() const noexcept override { return aa::transport::Kind::usb_aoa; }
    aa::core::Result<void> open(aa::core::CancellationToken token) override {
        token_ = token;
        return {};
    }
    aa::core::Result<void> send(std::span<const std::byte> bytes) override {
        std::lock_guard lock(state_->mutex);
        state_->sent.emplace_back(bytes.begin(), bytes.end());
        return {};
    }
    void set_io_checkpoint(std::function<void()> checkpoint) override {
        checkpoint_ = std::move(checkpoint);
    }
    aa::core::Result<std::vector<std::byte>> receive() override {
        std::unique_lock lock(state_->mutex);
        for (;;) {
            if (state_->closed) {
                return aa::Error{aa::ErrorCode::transport_closed};
            }
            if (state_->served < state_->inbound.size()) {
                auto frame = state_->inbound[state_->served];
                ++state_->served;
                lock.unlock();
                state_->cv.notify_all();
                return frame;
            }
            if (checkpoint_) {
                lock.unlock();
                checkpoint_();
                lock.lock();
            }
            state_->cv.wait_for(lock, token_, std::chrono::milliseconds{20}, [] { return false; });
            if (token_.stop_requested()) {
                return aa::Error{aa::ErrorCode::cancelled};
            }
        }
    }
    void close() noexcept override {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
        state_->cv.notify_all();
    }
  private:
    std::shared_ptr<AdmissionState> state_;
    aa::core::CancellationToken token_;
    std::function<void()> checkpoint_;
};

std::shared_ptr<AdmissionState> admission_link_state;
}

// Link-time seams keep the real CLI/thread loop under test without USB or audio hardware.
aa::core::Result<aa::app::WiredLink>
real_open_link(const aa::transport::UsbDeviceSelector &, const aa::transport::AoaAccessoryInfo &,
               const aa::trust::TrustStore &, const aa::core::Deadline &,
               aa::trust::WiredIdentity *)
    asm("__real__ZN2aa3app15open_wired_linkERKNS_9transport17UsbDeviceSelectorERKNS1_16AoaAccessoryInfoERKNS_5trust10TrustStoreERKNS_4core8DeadlineEPNS8_13WiredIdentityE");
aa::core::Result<aa::app::WiredLink>
runtime_open_link(const aa::transport::UsbDeviceSelector &, const aa::transport::AoaAccessoryInfo &,
                  const aa::trust::TrustStore &, const aa::core::Deadline &,
                  aa::trust::WiredIdentity *)
    asm("__wrap__ZN2aa3app15open_wired_linkERKNS_9transport17UsbDeviceSelectorERKNS1_16AoaAccessoryInfoERKNS_5trust10TrustStoreERKNS_4core8DeadlineEPNS8_13WiredIdentityE");
aa::core::Result<aa::app::WiredLink>
runtime_open_link(const aa::transport::UsbDeviceSelector &selector,
                  const aa::transport::AoaAccessoryInfo &info,
                  const aa::trust::TrustStore &store, const aa::core::Deadline &deadline,
                  aa::trust::WiredIdentity *attempted_identity) {
    const int attempt = ++runtime_connects;
    if (runtime_real_admission) {
        return real_open_link(selector, info, store, deadline, attempted_identity);
    }
    if (admission_link_state) {
        if (attempt != 1) {
            return aa::Error{aa::ErrorCode::transport_unavailable};
        }
        auto transport = std::make_unique<AdmissionTransport>(admission_link_state);
        {
            const std::lock_guard lock(admission_link_state->mutex);
            admission_link_state->created = true;
        }
        admission_link_state->cv.notify_all();
        return aa::app::WiredLink{std::move(transport), runtime_phone};
    }
    if (attempt == 1 || (runtime_recovers && attempt >= 4)) {
        return aa::app::WiredLink{std::make_unique<RuntimeTransport>(attempt == 1), runtime_phone};
    }
    return aa::Error{aa::ErrorCode::transport_unavailable};
}

aa::core::Result<std::unique_ptr<aa::transport::usb_adapter::AoaLinkFactory>>
real_link_factory() asm("__real__ZN2aa9transport11usb_adapter24make_libusb_link_factoryEv");
aa::core::Result<std::unique_ptr<aa::transport::usb_adapter::AoaLinkFactory>>
runtime_link_factory() asm("__wrap__ZN2aa9transport11usb_adapter24make_libusb_link_factoryEv");
aa::core::Result<std::unique_ptr<aa::transport::usb_adapter::AoaLinkFactory>>
runtime_link_factory() {
    if (!runtime_real_admission) {
        return real_link_factory();
    }
    auto factory = std::make_unique<aa::test::usb_aoa::FakeLinkFactory>(admission_state);
    factory->phone.serial = "SECOND-PRIVATE-USB";
    factory->accessory.serial = "SECOND-PRIVATE-AOA";
    factory->fail_switch = runtime_fail_before_admission;
    return std::unique_ptr<aa::transport::usb_adapter::AoaLinkFactory>{std::move(factory)};
}

std::unique_ptr<aa::transport::HandshakingCipher>
runtime_cipher(const std::function<bool()> &)
    asm("__wrap__ZN2aa9transport28make_client_handshake_cipherERKSt8functionIFbvEE");
std::unique_ptr<aa::transport::HandshakingCipher>
runtime_cipher(const std::function<bool()> &) {
    ++runtime_ciphers;
    return std::make_unique<wiredtest::FakeHandshakeCipher>();
}

aa::audio::detail::SinkOpenOutcome
runtime_sink(const aa::audio::detail::SinkOpenRequest &)
    asm("__wrap__ZN2aa5audio6detail17open_default_sinkERKNS1_15SinkOpenRequestE");
aa::audio::detail::SinkOpenOutcome
real_runtime_sink(const aa::audio::detail::SinkOpenRequest &)
    asm("__real__ZN2aa5audio6detail17open_default_sinkERKNS1_15SinkOpenRequestE");
aa::audio::detail::SinkOpenOutcome
runtime_sink(const aa::audio::detail::SinkOpenRequest &request) {
    if (runtime_sink_request_override) { return runtime_sink_request_override(request); }
    if (runtime_sink_override) { return {runtime_sink_override(), {}}; }
    return {std::make_unique<wiredtest::RecordingObservedSink>(
                std::make_shared<wiredtest::AudioObs>()), {}};
}

namespace {
struct AdmissionResult {
    int status{};
    bool watchdog_fired = false;
    int connects = -1;
    int ciphers = -1;
    int switches = -1;
    int claims = -1;
    std::string expected_label;
    std::string output;
};

AdmissionResult run_admission_child(bool pre_admission_failure) {
    if (const char *child_root = std::getenv("AA_TEST_ADMISSION_ROOT")) {
        const std::filesystem::path root{child_root};
        runtime_real_admission = true;
        runtime_fail_before_admission = std::string{std::getenv("AA_TEST_ADMISSION_FAILURE")} == "1";
        admission_state = std::make_shared<aa::test::usb_aoa::LinkState>();
        runtime_connects.store(0);
        runtime_ciphers.store(0);
        shutdown_observation = nullptr;
        g_running.store(true);
        const aa::trust::WiredIdentity second{
            "04e8", "6860", "SECOND-PRIVATE-USB", "SAMSUNG", "SAMSUNG_Android",
            "SECOND-PRIVATE-AOA", "1-2.2"};
        const auto label = aa::trust::redacted_label(aa::trust::TransportIdentity{second});
        std::vector<std::string> args{"receiver", "--port", "1-2.2", "--no-bus", "--store",
            (root / "approved.json").string(), "--video-socket", (root / "video").string(),
            "--input-socket", (root / "input").string(), "--duration-ms", "100"};
        std::vector<char *> argv;
        argv.reserve(args.size());
        for (auto &arg : args) {
            argv.push_back(arg.data());
        }
        const int code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
        {
            std::ofstream metrics(root / "metrics");
            metrics << runtime_connects.load() << ' ' << runtime_ciphers.load() << ' '
                    << admission_state->switches() << ' ' << admission_state->claims() << ' ' << label;
        }
        std::fflush(stdout);
        ::_exit(code);
    }
    AdmissionResult result;
    char directory[] = "/tmp/wired-admission-XXXXXX";
    const char *created = ::mkdtemp(directory);
    EXPECT_NE(created, nullptr);
    if (!created) {
        result.status = -1;
        return result;
    }
    const std::filesystem::path root{created};
    const auto store_path = root / "approved.json";
    aa::trust::ApprovedPhoneStore initial{store_path};
    EXPECT_TRUE(initial.load().has_value());
    const aa::trust::WiredIdentity first{
        "04e8", "6860", "FIRST-PRIVATE-USB", "SAMSUNG", "SAMSUNG_Android",
        "FIRST-PRIVATE-AOA", "1-2.1"};
    EXPECT_TRUE(initial.approve(aa::trust::TransportIdentity{first}).has_value());
    std::string root_env = "AA_TEST_ADMISSION_ROOT=" + root.string();
    std::string failure_env = pre_admission_failure ? "AA_TEST_ADMISSION_FAILURE=1"
                                                   : "AA_TEST_ADMISSION_FAILURE=0";
    std::vector<char *> environment;
    for (char **entry = ::environ; *entry != nullptr; ++entry) {
        environment.push_back(*entry);
    }
    environment.push_back(root_env.data());
    environment.push_back(failure_env.data());
    environment.push_back(nullptr);
    std::string executable = "/proc/self/exe";
    std::string filter = pre_admission_failure
        ? "--gtest_filter=WiredReceiver.PreAdmissionFailureEmitsNoAttemptAttribution"
        : "--gtest_filter=WiredReceiver.RejectedAttemptEmitsCapturedPseudonymAndPort";
    std::string repeat = "--gtest_repeat=1";
    char *child_argv[]{executable.data(), filter.data(), repeat.data(), nullptr};
    const int capture_fd = ::open((root / "stdout").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    EXPECT_GE(capture_fd, 0);
    std::fflush(nullptr);
    const pid_t child = ::fork();
    EXPECT_GE(child, 0);
    if (child == 0) {
        if (capture_fd < 0 || ::dup2(capture_fd, STDOUT_FILENO) < 0) {
            ::_exit(90);
        }
        ::close(capture_fd);
        ::execve(executable.c_str(), child_argv, environment.data());
        ::_exit(91);
    }
    if (capture_fd >= 0) {
        ::close(capture_fd);
    }
    if (child > 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        for (;;) {
            const auto waited = ::waitpid(child, &result.status, WNOHANG);
            if (waited == child) {
                break;
            }
            if (waited < 0 || std::chrono::steady_clock::now() >= deadline) {
                result.watchdog_fired = true;
                (void)::kill(child, SIGKILL);
                (void)::waitpid(child, &result.status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        std::ifstream capture(root / "stdout");
        result.output.assign(std::istreambuf_iterator<char>{capture}, {});
        std::ifstream metrics(root / "metrics");
        metrics >> result.connects >> result.ciphers >> result.switches >> result.claims
                >> result.expected_label;
    }
    std::filesystem::remove_all(root);
    return result;
}

void expect_admission_exit(const AdmissionResult &result, int claims) {
    ASSERT_FALSE(result.watchdog_fired) << "ADMISSION_WATCHDOG_FIRED";
    ASSERT_TRUE(WIFEXITED(result.status)) << result.status;
    EXPECT_EQ(WEXITSTATUS(result.status), 3) << result.output;
    EXPECT_EQ(result.connects, 1);
    EXPECT_EQ(result.switches, 1);
    EXPECT_EQ(result.claims, claims);
    EXPECT_EQ(result.ciphers, 0) << "rejected attempt reached TLS construction";
    EXPECT_EQ(result.output.find("\"event\":\"ready\""), std::string::npos);
    EXPECT_EQ(result.output.find("\"event\":\"active\""), std::string::npos);
    EXPECT_EQ(result.output.find("\"to_state\":\"active\""), std::string::npos);
}

std::vector<std::string> link_errors(const std::string &output) {
    std::vector<std::string> errors;
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        if (line.find("\"detail\":\"link\"") != std::string::npos) {
            errors.push_back(std::move(line));
        }
    }
    return errors;
}

TEST(WiredReceiver, RejectedAttemptEmitsCapturedPseudonymAndPort) {
    const aa::trust::WiredIdentity second{
        "04e8", "6860", "SECOND-PRIVATE-USB", "SAMSUNG", "SAMSUNG_Android",
        "SECOND-PRIVATE-AOA", "1-2.2"};
    const auto result = run_admission_child(false);
    const auto &label = result.expected_label;
    ASSERT_FALSE(label.empty());
    expect_admission_exit(result, 1);
    const auto errors = link_errors(result.output);
    ASSERT_EQ(errors.size(), 1U) << result.output;
    EXPECT_NE(errors[0].find("\"phone_id\":\"" + label + "\""), std::string::npos)
        << "CAPTURED_PSEUDONYM_MISSING: " << errors[0];
    EXPECT_NE(errors[0].find("\"port\":\"1-2.2\""), std::string::npos)
        << "SECOND_PORT_MISSING: " << errors[0];
    EXPECT_EQ(errors[0], "{\"event\":\"err\",\"detail\":\"link\",\"code\":\"transport_unauthorized_peer\","
                         "\"phone_id\":\"" + label + "\",\"port\":\"1-2.2\"}");
    const auto key = aa::trust::validated_key(aa::trust::TransportIdentity{second});
    ASSERT_TRUE(key.has_value());
    for (const auto &raw : {second.usb_serial, second.aoa_serial, second.aoa_manufacturer,
                            second.aoa_model, key.value(), std::string{"FIRST-PRIVATE"}}) {
        EXPECT_EQ(result.output.find(raw), std::string::npos) << "raw identity leaked: " << raw;
    }
}

TEST(WiredReceiver, PreAdmissionFailureEmitsNoAttemptAttribution) {
    const auto result = run_admission_child(true);
    expect_admission_exit(result, 0);
    const auto errors = link_errors(result.output);
    ASSERT_EQ(errors.size(), 1U) << result.output;
    EXPECT_EQ(errors[0], "{\"event\":\"err\",\"detail\":\"link\",\"code\":\"transport_unavailable\"}");
    EXPECT_EQ(errors[0].find("phone_id"), std::string::npos);
    EXPECT_EQ(errors[0].find("\"port\""), std::string::npos);
}

class RejectAudio final : public aa::audio::detail::ObservedAudioSink {
  public:
    aa::core::Result<void> write(const aa::audio::MixedChunk &) override {
        return aa::Error{aa::ErrorCode::transport_io};
    }
    aa::core::Result<void> drain(aa::core::Milliseconds) override { return {}; }
    aa::audio::SinkProbeStats probe_stats() const override { return {}; }
    aa::audio::RenderBarrierStats render_stats() const override { return {}; }
};

TEST(WiredReceiver, FailedAudioPumpAcksAdmissionAndEmitsNoRenderedDelivery) {
    ReceiverLogScope logging{g_running};
    ConcreteAudioRenderWorker sink{pcm_router_config(), [](const auto &config) {
        return aa::audio::detail::open_router_with_sink_for_test(config, std::make_unique<RejectAudio>());
    }};
    ASSERT_TRUE(sink.ready());
    const auto pcm = wiredtest::pcm_unit();
    aa::protocol::MessageView message{
        {aa::protocol::ChannelRole::media_audio, 1, {}}, pcm};
    capture_receiver_stdout();
    const auto result = sink.submit(message, {message.header.channel, 0, 1, 1});
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(sink.outstanding());
    // Admission credit (ACK-after-admission contract): the credit is earned
    // when the job is copied and reserved and survives the render failure
    // below; the failure still latches typed and no post-render delivery
    // evidence (audio_chunks) is ever emitted.
    const auto completion = sink.take_completion(std::chrono::steady_clock::now() +
                                                 std::chrono::seconds{2});
    ASSERT_TRUE(completion);
    EXPECT_EQ(*completion, (aa::channels::AvDeliveryId{message.header.channel, 0, 1, 1}));
    ASSERT_TRUE(sink.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    EXPECT_FALSE(sink.finish());
    const auto output = captured_receiver_stdout();
    ASSERT_TRUE(sink.failure());
    EXPECT_EQ(sink.failure()->code(), aa::ErrorCode::audio_sink_unavailable);
    EXPECT_EQ(output.find("audio_chunks"), std::string::npos)
        << "failed pump emitted rendered-delivery evidence: " << output;
}

struct RuntimeResult {
    int code;
    std::string output;
    std::chrono::steady_clock::duration elapsed;
};

RuntimeResult run_runtime(bool recovers, int duration_ms = 0, bool quiet = false) {
    char directory[] = "/tmp/wired-runtime-XXXXXX";
    const std::filesystem::path root{::mkdtemp(directory)};
    const auto path = root / "approved.json";
    aa::trust::ApprovedPhoneStore store{path};
    EXPECT_TRUE(store.load().has_value());
    const aa::trust::WiredIdentity identity{
        "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
    EXPECT_TRUE(store.approve(aa::trust::TransportIdentity{identity}).has_value());
    runtime_phone = aa::app::wired_phone_identity(identity).value();
    runtime_recovers = recovers;
    runtime_quiet = quiet;
    runtime_connects.store(0);
    runtime_ciphers.store(0);
    g_running.store(true);
    std::vector<std::string> args{"receiver", "--port", "1-2.1", "--no-bus", "--store",
                                  path.string(), "--video-socket", (root / "video").string(),
                                  "--input-socket", (root / "input").string(), "--duration-ms",
                                   duration_ms > 0 ? std::to_string(duration_ms)
                                                   : recovers ? "1000" : "6500"};
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto &arg : args) {
        argv.push_back(arg.data());
    }
    capture_receiver_stdout();
    const auto began = std::chrono::steady_clock::now();
    const int code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
    const auto elapsed = std::chrono::steady_clock::now() - began;
    auto output = captured_receiver_stdout();
    std::filesystem::remove_all(root);
    return {code, std::move(output), elapsed};
}

TEST(WiredReceiver, StdoutLogFailureConvertsCleanMainExitAndPreservesEarlyFailure) {
    // Given: real stdout ENOSPC, injected only after the runtime capture starts.
    const int full = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
    ASSERT_GE(full, 0);
    runtime_sink_request_override = [&](const auto &) -> aa::audio::detail::SinkOpenOutcome {
        EXPECT_EQ(::dup2(full, STDOUT_FILENO), STDOUT_FILENO);
        return {std::make_unique<wiredtest::RecordingObservedSink>(
            std::make_shared<wiredtest::AudioObs>()), {}};
    };
    // When: an otherwise clean actual-main invocation finishes with undelivered logs.
    const auto result = run_runtime(false, 20, true);
    runtime_sink_request_override = {};
    // Then: the owning wrapper converts 0 to 1 and an early nonzero stays nonzero.
    EXPECT_EQ(result.code, 1);
    EXPECT_FALSE(g_running.load());
    {
        StdoutRestore restore;
        EXPECT_GE(restore.saved, 0);
        EXPECT_EQ(::dup2(full, STDOUT_FILENO), STDOUT_FILENO);
        std::string executable = "receiver", port_flag = "--port", port = "1-2.1";
        std::string store_flag = "--store", path = "/dev/null/invalid-approved.json";
        char *args[]{executable.data(), port_flag.data(), port.data(), store_flag.data(), path.data()};
        g_running.store(true);
        const int code = wired_receiver_main(5, args);
        restore.restore();
        EXPECT_EQ(code, 3);
        EXPECT_FALSE(g_running.load());
    }
    ::close(full);
    EXPECT_EQ(aa::wired_receiver::active_stdout_log.load(), nullptr);
    g_running.store(true);
}

TEST(WiredReceiver, TransientLinkLossRetriesUntilFreshSessionRecovers) {
    const auto result = run_runtime(true);
    EXPECT_EQ(result.code, 0) << result.output;
    EXPECT_GE(runtime_connects.load(), 4) << "reconnect stopped after one immediate attempt";
    EXPECT_EQ(runtime_ciphers.load(), 2);
    const auto active = result.output.find("\"to_state\":\"active\"");
    ASSERT_NE(active, std::string::npos) << result.output;
    EXPECT_NE(result.output.find("\"to_state\":\"active\"", active + 1), std::string::npos)
        << result.output;
}

TEST(WiredReceiver, PermanentLinkLossExitsNonzeroWithinWiredRecoveryBudget) {
    const auto result = run_runtime(false);
    EXPECT_EQ(result.code, 1) << "dead session returned clean success: " << result.output;
    EXPECT_GE(runtime_connects.load(), 3);
    EXPECT_EQ(runtime_ciphers.load(), 1);
    EXPECT_LT(result.elapsed, std::chrono::milliseconds{5500});
}

TEST(WiredReceiver, ActiveFramesShutdownOrdersStopAndReset) {
    namespace faults = aa::trust::test::faults;
    for (const int stimulus : {0, SIGINT, SIGTERM}) {
        SCOPED_TRACE(stimulus == 0 ? "duration" : stimulus == SIGINT ? "SIGINT" : "SIGTERM");
        char directory[] = "/tmp/wired-shutdown-runtime-XXXXXX";
        const std::filesystem::path root{::mkdtemp(directory)};
        const auto path = root / "approved.json";
        aa::trust::ApprovedPhoneStore initial{path};
        ASSERT_TRUE(initial.load().has_value());
        const aa::trust::WiredIdentity identity{
            "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
        ASSERT_TRUE(initial.approve(aa::trust::TransportIdentity{identity}).has_value());
        const auto read_store = [&] {
            std::ifstream file(path);
            return std::string(std::istreambuf_iterator<char>{file}, {});
        };
        const auto store_before = read_store();
        runtime_phone = aa::app::wired_phone_identity(identity).value();
        runtime_quiet = false;
        runtime_recovers = false;
        runtime_connects.store(0);
        runtime_ciphers.store(0);
        runtime_closed.store(false);
        g_running.store(true);
        ShutdownObservation observed;
        shutdown_observation = &observed;
        std::atomic<int> mutations{0};
        faults::arm(faults::Fault::none);
        faults::set_observer([&] { ++mutations; });
        std::vector<std::string> args{"receiver", "--port", "1-2.1", "--store", path.string(),
            "--video-socket", (root / "video").string(), "--input-socket", (root / "input").string(),
            "--duration-ms", stimulus == 0 ? "1000" : "0"};
        std::vector<char *> argv;
        argv.reserve(args.size());
        for (auto &arg : args) {
            argv.push_back(arg.data());
        }
        capture_receiver_stdout();
        const auto began = ShutdownObservation::Clock::now();
        int code = -1;
        std::thread receiver([&] {
            code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
        });
        std::atomic<bool> watchdog_fired{false};
        std::jthread watchdog([&](const std::stop_token &stop) {
            std::mutex mutex;
            std::condition_variable_any cv;
            std::unique_lock lock(mutex);
            cv.wait_for(lock, stop, std::chrono::seconds{5}, [] { return false; });
            if (!stop.stop_requested()) {
                watchdog_fired.store(true);
                g_running.store(false);
                const std::lock_guard state_lock(observed.state);
                observed.release_all();
            }
        });
        bool traffic = false;
        bool publication = false;
        bool cleanup = false;
        bool challenge = false;
        int signal_result = -1;
        {
            std::unique_lock lock(observed.state);
            traffic = observed.wait(lock, [&] { return observed.receive_held; });
            if (stimulus != 0 && traffic) {
                signal_result = ::pthread_kill(receiver.native_handle(), stimulus);
            }
            publication = observed.wait(lock, [&] { return observed.published; });
            observed.release_receive = true;
            observed.cv.notify_all();
            cleanup = observed.wait(lock, [&] { return observed.cleanup_held; });
            observed.release_main = true;
            observed.cv.notify_all();
            challenge = observed.wait(lock, [&] {
                return observed.main_attempt || observed.unprotected_stop;
            });
            observed.release_cleanup = true;
            observed.cv.notify_all();
        }
        receiver.join();
        const auto ended = ShutdownObservation::Clock::now();
        watchdog.request_stop();
        watchdog.join();
        const auto output = captured_receiver_stdout();
        faults::set_observer({});
        faults::disarm();
        shutdown_observation = nullptr;
        EXPECT_TRUE(traffic);
        EXPECT_TRUE(publication);
        EXPECT_TRUE(cleanup);
        EXPECT_TRUE(challenge);
        EXPECT_FALSE(observed.gate_failed);
        EXPECT_FALSE(watchdog_fired.load());
        if (stimulus == 0) {
            EXPECT_GE(observed.published_at - began, std::chrono::milliseconds{1000});
        } else {
            EXPECT_EQ(signal_result, 0);
        }
        EXPECT_EQ(observed.pings, 4);
        EXPECT_EQ(observed.replies, 4);
        EXPECT_EQ(observed.final_receives, 1);
        EXPECT_EQ(observed.resets, 1);
        EXPECT_TRUE(observed.reset_ok);
        EXPECT_EQ(observed.stops, 1);
        EXPECT_EQ(observed.joins, 1);
        EXPECT_NE(observed.reset_owner, observed.dispatcher);
        EXPECT_FALSE(observed.unprotected_stop) << "MAIN_STOP_WITHOUT_SESSION_MUTEX";
        if (observed.unprotected_stop) {
            EXPECT_TRUE(observed.unprotected_during_cleanup);
        } else {
            EXPECT_TRUE(observed.stop_owned) << "MAIN_STOP_WITHOUT_SESSION_MUTEX";
            const auto position = [&](const char *name, std::thread::id thread,
                                      std::size_t after = 0) {
                for (auto i = after; i < observed.trace.size(); ++i) {
                    if (observed.trace[i].name == name && observed.trace[i].thread == thread) {
                        return i;
                    }
                }
                return observed.trace.size();
            };
            const auto reset = position("reset", observed.reset_owner);
            const auto release = position("release", observed.reset_owner, reset + 1);
            const auto acquire = position("acquire", observed.dispatcher, release + 1);
            const auto stop = position("stop", observed.dispatcher, acquire + 1);
            const auto unlock = position("release", observed.dispatcher, stop + 1);
            const auto join = position("join", observed.dispatcher, unlock + 1);
            EXPECT_LT(reset, release);
            EXPECT_LT(release, acquire);
            EXPECT_LT(acquire, stop);
            EXPECT_LT(stop, unlock);
            EXPECT_LT(unlock, join);
            EXPECT_LT(join, observed.trace.size());
        }
        EXPECT_FALSE(observed.join_owned);
        EXPECT_LT(ended - observed.published_at, std::chrono::seconds{3});
        EXPECT_EQ(code, 0) << output;
        EXPECT_EQ(runtime_connects.load(), 1);
        EXPECT_EQ(runtime_ciphers.load(), 1);
        EXPECT_TRUE(runtime_closed.load());
        EXPECT_EQ(observed.post_close_io, 0);
        EXPECT_EQ(mutations.load(), 0);
        EXPECT_EQ(read_store(), store_before);
        aa::trust::ApprovedPhoneStore persisted{path};
        EXPECT_TRUE(persisted.load().has_value());
        EXPECT_EQ(persisted.lookup(runtime_phone), aa::trust::Decision::approved);
        const auto active = output.find("\"to_state\":\"active\"");
        const auto stopping = output.find("\"to_state\":\"stopping\"", active);
        const auto disconnected = output.find("\"to_state\":\"disconnected\"", stopping);
        EXPECT_NE(active, std::string::npos) << output;
        EXPECT_NE(stopping, std::string::npos) << output;
        EXPECT_NE(disconnected, std::string::npos) << output;
        EXPECT_LT(active, stopping);
        EXPECT_LT(stopping, disconnected);
        EXPECT_EQ(output.find("\"to_state\":\"reconnecting\""), std::string::npos) << output;
        EXPECT_EQ(output.find("\"to_state\":\"failed\""), std::string::npos) << output;
        EXPECT_EQ(output.find("\"event\":\"err\""), std::string::npos) << output;
        std::printf("G1 stimulus=%s pings=%d replies=%d final_receive=%d resets=%d "
                    "stop_owned=%d join_owned=%d gate_failed=%d watchdog=%d\n",
                    stimulus == 0 ? "duration" : stimulus == SIGINT ? "SIGINT" : "SIGTERM",
                    observed.pings, observed.replies, observed.final_receives, observed.resets,
                    observed.stop_owned, observed.join_owned, observed.gate_failed,
                    watchdog_fired.load());
        std::filesystem::remove_all(root);
    }
}

TEST(WiredReceiver, ForgetPhoneUsesOwnerQueueAndDisconnectsQuietSession) {
    namespace bus = aa::ipc::dbus_adapter;
    namespace faults = aa::trust::test::faults;
    char directory[] = "/tmp/wired-revoke-runtime-XXXXXX";
    const std::filesystem::path root{::mkdtemp(directory)};
    const auto path = root / "approved.json";
    aa::trust::ApprovedPhoneStore initial{path};
    ASSERT_TRUE(initial.load().has_value());
    const aa::trust::WiredIdentity identity{
        "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
    const auto approved = initial.approve(aa::trust::TransportIdentity{identity});
    ASSERT_TRUE(approved.has_value());
    runtime_phone = aa::app::wired_phone_identity(identity).value();
    runtime_quiet = true;
    runtime_recovers = false;
    runtime_connects.store(0);
    runtime_ciphers.store(0);
    runtime_waiting.store(false);
    runtime_closed.store(false);
    g_running.store(true);
    std::vector<std::string> args{"receiver", "--port", "1-2.1", "--store", path.string(),
        "--video-socket", (root / "video").string(), "--input-socket", (root / "input").string(),
        "--duration-ms", "5000"};
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    std::vector<std::thread::id> mutations;
    faults::arm(faults::Fault::none);
    faults::set_observer([&] { mutations.push_back(std::this_thread::get_id()); });
    capture_receiver_stdout();
    int code = -1;
    std::thread::id dispatcher;
    std::thread receiver([&] {
        dispatcher = std::this_thread::get_id();
        code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
    });
    std::atomic<bool> watchdog_fired{false};
    std::jthread watchdog([&](const std::stop_token& stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait_for(lock, stop, std::chrono::seconds{3}, [] { return false; });
        if (!stop.stop_requested()) {
            watchdog_fired.store(true);
            g_running.store(false);
        }
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!runtime_waiting.load() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const bool was_quiet = runtime_waiting.load();
    auto client = bus::BusConnection::connect_session();
    aa::core::Result<bus::ParsedMessage> reply{aa::Error{aa::ErrorCode::internal}};
    bool registered = false;
    bool closed_at_reply = false;
    if (client && was_quiet) {
        const aa::ipc::BusName consumer{"org.custom.AndroidAutoReceiverTest.Consumer"};
        const aa::ipc::BusName service{"org.custom.AndroidAutoReceiver"};
        auto named = client.value().request_name(consumer);
        auto owner = client.value().get_name_owner(service);
        if (named && owner) {
            bus::MessageWriter registration;
            registration.destination(service.value).path("/org/custom/AndroidAutoReceiver")
                .interface_name("org.custom.AndroidAutoReceiver1").member("RegisterConsumer")
                .body_signature("s").put_string(consumer.value);
            auto serial = client.value().send_message(bus::MessageType::method_call, registration);
            if (serial) {
                auto result = client.value().await_reply(serial.value(), owner.value());
                registered = result && result.value().type == bus::MessageType::method_return;
            }
            if (registered) {
                bus::MessageWriter forget;
                forget.destination(service.value).path("/org/custom/AndroidAutoReceiver")
                    .interface_name("org.custom.AndroidAutoReceiver1").member("ForgetPhone")
                    .body_signature("t").put_u64(approved.value().value);
                auto sent = client.value().send_message(bus::MessageType::method_call, forget);
                if (sent) {
                    reply = client.value().await_reply(sent.value(), owner.value());
                    closed_at_reply = runtime_closed.load();
                }
            }
        }
    }
    if (!registered || !reply) {
        g_running.store(false);
    }
    receiver.join();
    watchdog.request_stop();
    watchdog.join();
    const auto output = captured_receiver_stdout();
    faults::set_observer({});
    faults::disarm();
    EXPECT_FALSE(watchdog_fired.load()) << output;
    EXPECT_TRUE(was_quiet);
    EXPECT_TRUE(registered);
    ASSERT_TRUE(reply.has_value()) << output;
    EXPECT_EQ(reply.value().type, bus::MessageType::error_reply);
    bus::Cursor external_error{reply.value().body};
    EXPECT_EQ(external_error.read_string().value(), aa::Error{aa::ErrorCode::helper_unavailable}.message());
    EXPECT_TRUE(closed_at_reply);
    EXPECT_EQ(code, 1) << output;
    EXPECT_EQ(runtime_connects.load(), 1);
    EXPECT_EQ(runtime_ciphers.load(), 1);
    EXPECT_NE(output.find("\"to_state\":\"failed\""), std::string::npos) << output;
    EXPECT_EQ(output.find("\"detail\":\"poll\""), std::string::npos) << output;
    ASSERT_FALSE(mutations.empty());
    {
        const std::lock_guard lock(runtime_owner_mutex);
        for (const auto thread : mutations) {
            EXPECT_EQ(thread, runtime_io_owner);
            EXPECT_NE(thread, dispatcher);
        }
    }
    aa::trust::ApprovedPhoneStore persisted{path};
    ASSERT_TRUE(persisted.load().has_value());
    EXPECT_EQ(persisted.lookup(runtime_phone), aa::trust::Decision::unknown);
    std::filesystem::remove_all(root);
    runtime_quiet = false;
}

struct RequiredRun {
    int code{0};
    std::string output;
    std::chrono::steady_clock::duration elapsed{};
    int sends{0};
    int connects{0};
    bool watchdog_fired{false};
    bool negotiation_timeout{false};
};

RequiredRun run_required_mode(bool require_flag, bool attach_consumer,
                              std::int64_t duration_ms = 2500) {
    RequiredFailureObservation failure;
    required_failure_observation = &failure;
    char directory[] = "/tmp/wired-required-XXXXXX";
    const std::filesystem::path root{::mkdtemp(directory)};
    const auto path = root / "approved.json";
    aa::trust::ApprovedPhoneStore store{path};
    EXPECT_TRUE(store.load().has_value());
    const aa::trust::WiredIdentity identity{
        "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
    EXPECT_TRUE(store.approve(aa::trust::TransportIdentity{identity}).has_value());
    runtime_phone = aa::app::wired_phone_identity(identity).value();
    runtime_recovers = false;
    runtime_quiet = true;
    runtime_connects.store(0);
    runtime_ciphers.store(0);
    runtime_sends.store(0);
    g_running.store(true);
    const auto video_socket = root / "video";
    std::vector<std::string> args{"receiver", "--port", "1-2.1", "--no-bus", "--store",
                                  path.string(), "--video-socket", video_socket.string(),
                                  "--input-socket", (root / "input").string(), "--duration-ms",
                                  std::to_string(duration_ms)};
    if (require_flag) {
        args.push_back("--require-video-consumer");
    }
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto &arg : args) {
        argv.push_back(arg.data());
    }
    capture_receiver_stdout();
    const auto began = std::chrono::steady_clock::now();
    int code = -1;
    std::thread receiver([&] {
        code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
    });
    std::atomic<bool> watchdog_fired{false};
    std::jthread watchdog([&](const std::stop_token &stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait_for(lock, stop, std::chrono::seconds{20}, [] { return false; });
        if (!stop.stop_requested()) {
            watchdog_fired.store(true);
            g_running.store(false);
        }
    });
    std::optional<aa::ipc::VideoConsumer> consumer;
    if (attach_consumer) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (!std::filesystem::exists(video_socket) &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        aa::core::Result<aa::ipc::VideoConsumer> client{aa::ipc::VideoConsumer{}};
        std::thread connector([&] {
            client = aa::ipc::connect_video_consumer(
                aa::ipc::VideoSocketPath{video_socket.string()},
                aa::ipc::VideoClientOptions{{}, aa::ipc::current_uid(),
                                            aa::core::Milliseconds{3000}});
        });
        connector.join();
        if (client.has_value()) {
            consumer.emplace(std::move(client.value()));
        }
    }
    receiver.join();
    const auto elapsed = std::chrono::steady_clock::now() - began;
    watchdog.request_stop();
    watchdog.join();
    auto output = captured_receiver_stdout();
    if (consumer.has_value()) {
        consumer.value().close();
    }
    std::filesystem::remove_all(root);
    runtime_quiet = false;
    required_failure_observation = nullptr;
    return {code, std::move(output), elapsed, runtime_sends.load(), runtime_connects.load(),
            watchdog_fired.load(), failure.negotiation_timeout};
}

TEST(WiredReceiver, RequiredVideoConsumerDefersUpstreamUntilProducerInstalled) {
    const auto withheld = run_required_mode(true, false);
    EXPECT_EQ(withheld.sends, 0)
        << "upstream protocol progressed before a video producer was installed: "
        << withheld.output;
    EXPECT_EQ(withheld.output.find("\"event\":\"active\""), std::string::npos) << withheld.output;

    const auto attached = run_required_mode(true, true);
    EXPECT_GT(attached.sends, 0)
        << "upstream did not proceed after a video producer was installed: " << attached.output;
}

TEST(WiredReceiver, RequiredVideoConsumerDurationStopIsCleanWithoutNegotiationFailure) {
    const auto withheld = run_required_mode(true, false, 2500);
    EXPECT_FALSE(withheld.watchdog_fired);
    EXPECT_EQ(withheld.code, 0) << withheld.output;
    EXPECT_FALSE(withheld.negotiation_timeout);
    EXPECT_GE(withheld.elapsed, std::chrono::milliseconds{2500});
    EXPECT_EQ(withheld.sends, 0) << withheld.output;
    EXPECT_EQ(withheld.output.find("\"event\":\"active\""), std::string::npos) << withheld.output;
    EXPECT_EQ(withheld.connects, 1) << "admission timeout reconnected/reset the budget";
    EXPECT_EQ(withheld.output.find("\"to_state\":\"failed\""), std::string::npos) << withheld.output;
    EXPECT_EQ(withheld.output.find("\"event\":\"err\""), std::string::npos) << withheld.output;
    EXPECT_LT(withheld.elapsed, std::chrono::milliseconds{2500 + 1500}) << withheld.output;
}

TEST(WiredReceiver, RequiredVideoConsumerNaturallyExpiresOriginal15SecondNegotiation) {
    // Given no consumer and a runtime longer than the unchanged negotiation phase.
    // When the actual main runs, only its owner tick may produce this failure.
    const auto withheld = run_required_mode(true, false, 19000);
    // Then cancellation, runtime expiry, reconnect and watchdog cannot count as proof.
    EXPECT_FALSE(withheld.watchdog_fired) << "NEGOTIATION_WATCHDOG_FIRED";
    EXPECT_EQ(withheld.code, 1) << withheld.output;
    EXPECT_EQ(withheld.sends, 0) << withheld.output;
    EXPECT_EQ(withheld.connects, 1);
    EXPECT_GE(withheld.elapsed, std::chrono::seconds{15});
    EXPECT_LT(withheld.elapsed, std::chrono::seconds{18});
    EXPECT_NE(withheld.output.find("\"to_state\":\"failed\""), std::string::npos) << withheld.output;
    EXPECT_TRUE(withheld.negotiation_timeout) << withheld.output;
    EXPECT_EQ(withheld.output.find("\"event\":\"active\""), std::string::npos) << withheld.output;
    EXPECT_EQ(withheld.output.find("\"to_state\":\"active\""), std::string::npos) << withheld.output;
    EXPECT_EQ(withheld.output.find("\"to_state\":\"reconnecting\""), std::string::npos) << withheld.output;
}

TEST(WiredReceiver, RequiredVideoConsumerRejectsAdmissionAfterRuntimeDeadline) {
    for (const auto edge : {DeadlineObservation::Edge::dispatch, DeadlineObservation::Edge::accepted}) {
        SCOPED_TRACE(edge == DeadlineObservation::Edge::dispatch ? "dispatch" : "accept/limits");
        DeadlineObservation observed{edge};
        deadline_observation = &observed;
        const auto result = run_required_mode(true, true, 1000);
        deadline_observation = nullptr;
        EXPECT_TRUE(observed.crossed);
        EXPECT_FALSE(result.watchdog_fired);
        EXPECT_EQ(result.code, 0) << result.output;
        EXPECT_EQ(result.sends, 0) << result.output;
        EXPECT_EQ(result.connects, 1);
        EXPECT_EQ(result.output.find("\"event\":\"video_accepted\""), std::string::npos)
            << "LATE_PRODUCER_INSTALLED: " << result.output;
        EXPECT_EQ(result.output.find("\"to_state\":\"active\""), std::string::npos) << result.output;
    }
}

TEST(WiredReceiver, RequiredVideoConsumerOwnerRejectsInitialVersionAfterRuntimeDeadline) {
    DeadlineObservation observed{DeadlineObservation::Edge::owner};
    deadline_observation = &observed;
    const auto result = run_required_mode(true, true, 1000);
    deadline_observation = nullptr;
    EXPECT_TRUE(observed.crossed);
    EXPECT_TRUE(observed.owner_done);
    EXPECT_FALSE(observed.gate_failed);
    EXPECT_FALSE(result.watchdog_fired);
    EXPECT_EQ(result.code, 0) << result.output;
    EXPECT_EQ(result.connects, 1);
    EXPECT_NE(result.output.find("\"event\":\"video_accepted\""), std::string::npos) << result.output;
    EXPECT_EQ(result.sends, 0) << "OWNER_SENT_AFTER_RUNTIME_DEADLINE: " << result.output;
    EXPECT_EQ(result.output.find("\"to_state\":\"active\""), std::string::npos) << result.output;
}

TEST(WiredReceiver, DefaultModeWithoutConsumerStillNegotiates) {
    const auto headless = run_required_mode(false, false, 1500);
    EXPECT_GT(headless.sends, 0)
        << "default headless mode no longer sends upstream without a consumer: "
        << headless.output;
}

TEST(WiredReceiver, RequiredVideoConsumerForgetPhoneWhileGatedCompletesWithoutDeadlock) {
    namespace bus = aa::ipc::dbus_adapter;
    char directory[] = "/tmp/wired-required-revoke-XXXXXX";
    const std::filesystem::path root{::mkdtemp(directory)};
    const auto path = root / "approved.json";
    aa::trust::ApprovedPhoneStore initial{path};
    ASSERT_TRUE(initial.load().has_value());
    const aa::trust::WiredIdentity identity{
        "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
    const auto approved = initial.approve(aa::trust::TransportIdentity{identity});
    ASSERT_TRUE(approved.has_value());
    runtime_phone = aa::app::wired_phone_identity(identity).value();
    runtime_quiet = true;
    runtime_recovers = false;
    runtime_connects.store(0);
    runtime_ciphers.store(0);
    runtime_sends.store(0);
    g_running.store(true);
    const auto video_socket = root / "video";
    std::vector<std::string> args{"receiver", "--port", "1-2.1", "--store", path.string(),
                                  "--video-socket", video_socket.string(),
                                  "--input-socket", (root / "input").string(),
                                  "--duration-ms", "5000", "--require-video-consumer"};
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto &arg : args) {
        argv.push_back(arg.data());
    }
    capture_receiver_stdout();
    int code = -1;
    std::thread receiver([&] {
        code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
    });
    std::atomic<bool> watchdog_fired{false};
    std::jthread watchdog([&](const std::stop_token &stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait_for(lock, stop, std::chrono::seconds{4}, [] { return false; });
        if (!stop.stop_requested()) {
            watchdog_fired.store(true);
            g_running.store(false);
        }
    });
    const auto socket_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (!std::filesystem::exists(video_socket) &&
           std::chrono::steady_clock::now() < socket_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    auto client = bus::BusConnection::connect_session();
    aa::core::Result<bus::ParsedMessage> reply{aa::Error{aa::ErrorCode::internal}};
    bool registered = false;
    if (client.has_value()) {
        const aa::ipc::BusName consumer{"org.custom.AndroidAutoReceiverTest.Consumer"};
        const aa::ipc::BusName service{"org.custom.AndroidAutoReceiver"};
        auto owner = client.value().get_name_owner(service);
        const auto owner_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (!owner.has_value() && std::chrono::steady_clock::now() < owner_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            owner = client.value().get_name_owner(service);
        }
        auto named = client.value().request_name(consumer);
        if (named.has_value() && owner.has_value()) {
            bus::MessageWriter registration;
            registration.destination(service.value)
                .path("/org/custom/AndroidAutoReceiver")
                .interface_name("org.custom.AndroidAutoReceiver1")
                .member("RegisterConsumer")
                .body_signature("s")
                .put_string(consumer.value);
            auto serial = client.value().send_message(bus::MessageType::method_call, registration);
            if (serial.has_value()) {
                auto result = client.value().await_reply(serial.value(), owner.value());
                registered = result.has_value() &&
                             result.value().type == bus::MessageType::method_return;
            }
            if (registered) {
                bus::MessageWriter forget;
                forget.destination(service.value)
                    .path("/org/custom/AndroidAutoReceiver")
                    .interface_name("org.custom.AndroidAutoReceiver1")
                    .member("ForgetPhone")
                    .body_signature("t")
                    .put_u64(approved.value().value);
                auto sent = client.value().send_message(bus::MessageType::method_call, forget);
                if (sent.has_value()) {
                    reply = client.value().await_reply(sent.value(), owner.value());
                }
            }
        }
    }
    if (!registered || !reply.has_value()) {
        g_running.store(false);
    }
    receiver.join();
    watchdog.request_stop();
    watchdog.join();
    const auto output = captured_receiver_stdout();
    EXPECT_FALSE(watchdog_fired.load()) << "ForgetPhone deadlocked while admission was gated: "
                                        << output;
    EXPECT_TRUE(registered);
    ASSERT_TRUE(reply.has_value()) << output;
    EXPECT_EQ(reply.value().type, bus::MessageType::error_reply);
    bus::Cursor external_error{reply.value().body};
    EXPECT_EQ(external_error.read_string().value(), aa::Error{aa::ErrorCode::helper_unavailable}.message());
    aa::trust::ApprovedPhoneStore persisted{path};
    ASSERT_TRUE(persisted.load().has_value());
    EXPECT_EQ(persisted.lookup(runtime_phone), aa::trust::Decision::unknown)
        << "consent bypass: ForgetPhone did not revoke while admission was gated";
    std::filesystem::remove_all(root);
    runtime_quiet = false;
}

TEST(WiredReceiver, InitialConfigIdrReachesConsumerExactlyWithFollowingNonIdr) {
    ReceiverLogScope logging{g_running};
    namespace p = aa::protocol;
    namespace tr = aa::transport;
    aa::session::test::MapTrustStore trust;
    const aa::trust::PhoneIdentity phone{"synthetic-idr-peer"};
    trust.set(phone, aa::trust::Decision::approved);
    aa::session::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    wiredtest::ScriptedTransport transport;
    std::vector<std::string> logs;
    VideoOutlet video{[&](const std::string &line) { logs.push_back(line); }};
    auto pair = videotest::make_pair({});
    ASSERT_TRUE(pair);
    video.install(std::move(pair.value().producer));
    ConcreteAudioRenderWorker audio{pcm_router_config(), [](const auto &config) {
        return aa::audio::detail::open_router_with_sink_for_test(
            config, std::make_unique<ReceiverProfileSink>());
    }};
    ASSERT_TRUE(audio.ready());
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    ASSERT_TRUE(session.configure(
        service_descriptors(aa::input::CapabilityDescriptor::bench_defaults())));
    ASSERT_TRUE(register_av_sinks(session, video, audio));
    session.attach_transport(&transport);
    auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
    auto *cipher = owned.get();
    session.attach_cipher(std::move(owned));
    wiredtest::walk_auth(session, transport);
    ASSERT_TRUE(session.authenticated());
    ASSERT_TRUE(session.announce_services());
    const auto receive = [&](std::vector<std::byte> payload) -> aa::core::Result<bool> {
        auto frames = tr::encode_message({99, tr::MessageKind::specific, std::move(payload)},
                                         tr::Encryption::encrypted, cipher);
        if (!frames) {
            return frames.error();
        }
        for (auto &frame : frames.value()) {
            transport.push_inbound(std::move(frame));
        }
        return session.poll_transport();
    };
    ASSERT_TRUE(receive(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value()));
    ASSERT_TRUE(receive(p::encode(p::AvSinkStart{-7, 0}).value()));
    auto config = std::vector<std::byte>{std::byte{0}, std::byte{1}, std::byte{0},
                                         std::byte{0}, std::byte{0}, std::byte{1},
                                         std::byte{0x67}, std::byte{0x42}};
    ASSERT_TRUE(receive(config));
    std::vector<std::byte> idr(10, std::byte{0x55});
    idr[0] = idr[1] = std::byte{0};
    idr.insert(idr.end(), wiredtest::kVideoIdr.begin(), wiredtest::kVideoIdr.end());
    clock.set(aa::core::Nanoseconds{987654});
    ASSERT_TRUE(receive(idr));
    ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
    const auto &unit = pair.value().consumer.access_unit();
    EXPECT_EQ(unit.timestamp_ns, 987654U);
    EXPECT_TRUE(unit.idr);
    std::vector<std::uint8_t> expected;
    for (auto it = config.begin() + 2; it != config.end(); ++it) {
        expected.push_back(std::to_integer<std::uint8_t>(*it));
    }
    for (const auto byte : wiredtest::kVideoIdr) {
        expected.push_back(std::to_integer<std::uint8_t>(byte));
    }
    EXPECT_EQ(unit.payload, expected);
    auto non_idr = idr;
    non_idr[14] = std::byte{0x41};
    for (std::uint64_t follow = 1; follow <= 3; ++follow) {
        clock.set(aa::core::Nanoseconds{static_cast<std::int64_t>(987654 + follow * 100)});
        ASSERT_TRUE(receive(non_idr));
        ASSERT_TRUE(pair.value().consumer.receive(Milliseconds{500}));
        const auto &next = pair.value().consumer.access_unit();
        EXPECT_EQ(next.timestamp_ns, 987654U + follow * 100U);
        EXPECT_FALSE(next.idr);
        EXPECT_FALSE(next.payload.empty());
    }
    video.close();
}

std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::unique_ptr<wiredtest::FakeHandshakeCipher> active_test_cipher() {
    auto cipher = std::make_unique<wiredtest::FakeHandshakeCipher>();
    (void)cipher->step_handshake();
    (void)cipher->step_handshake();
    return cipher;
}

std::vector<std::byte> av_data_message(std::uint64_t peer_timestamp,
                                       const std::vector<std::byte> &media) {
    std::vector<std::byte> payload{std::byte{0}, std::byte{0}};
    for (int shift = 56; shift >= 0; shift -= 8) {
        payload.push_back(static_cast<std::byte>((peer_timestamp >> shift) & 0xffU));
    }
    payload.insert(payload.end(), media.begin(), media.end());
    return payload;
}

struct ReceiverRunGuard {
    std::thread worker;
    ReceiverRunGuard() = default;
    ~ReceiverRunGuard() {
        if (worker.joinable()) {
            g_running.store(false);
            worker.join();
        }
    }
    ReceiverRunGuard(const ReceiverRunGuard &) = delete;
    ReceiverRunGuard &operator=(const ReceiverRunGuard &) = delete;
};

struct StdoutCaptureGuard {
    std::string output;
    bool released = false;
    StdoutCaptureGuard() { capture_receiver_stdout(); }
    ~StdoutCaptureGuard() {
        if (!released) {
            auto text = captured_receiver_stdout();
            if (testing::Test::HasFailure()) {
                std::fputs(text.c_str(), stdout);
            }
        }
    }
    std::string release() {
        released = true;
        output = captured_receiver_stdout();
        return output;
    }
    StdoutCaptureGuard(const StdoutCaptureGuard &) = delete;
    StdoutCaptureGuard &operator=(const StdoutCaptureGuard &) = delete;
};

TEST(WiredReceiver, RequiredVideoConsumerAdmissionDeliversSoleInitialConfigIdr) {
    namespace p = aa::protocol;
    namespace tr = aa::transport;
    char directory[] = "/tmp/wired-gate-deliver-XXXXXX";
    const std::filesystem::path root{::mkdtemp(directory)};
    const auto store_path = root / "approved.json";
    aa::trust::ApprovedPhoneStore store{store_path};
    ASSERT_TRUE(store.load().has_value());
    const aa::trust::WiredIdentity identity{
        "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
    ASSERT_TRUE(store.approve(aa::trust::TransportIdentity{identity}).has_value());
    runtime_phone = aa::app::wired_phone_identity(identity).value();
    runtime_recovers = false;
    runtime_quiet = false;
    runtime_connects.store(0);
    runtime_ciphers.store(0);
    runtime_sends.store(0);
    g_running.store(true);
    auto state = std::make_shared<AdmissionState>();
    admission_link_state = state;
    std::vector<std::string> args{"receiver", "--port", "1-2.1", "--no-bus", "--store",
        store_path.string(), "--video-socket", (root / "video").string(), "--input-socket",
        (root / "input").string(), "--duration-ms", "6000", "--require-video-consumer"};
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto &arg : args) {
        argv.push_back(arg.data());
    }
    StdoutCaptureGuard capture;
    int code = -1;
    ReceiverRunGuard guard;
    guard.worker =
        std::thread([&] { code = wired_receiver_main(static_cast<int>(argv.size()), argv.data()); });
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->cv.wait_for(lock, std::chrono::seconds{3},
                                       [&] { return state->created; }))
            << "LINK_NEVER_CREATED";
    }
    auto cipher = active_test_cipher();
    const auto push_plain = [&](const std::vector<std::byte> &frame) {
        const std::lock_guard lock(state->mutex);
        state->inbound.push_back(frame);
        state->cv.notify_all();
    };
    const auto push_media = [&](std::vector<std::byte> payload) {
        auto frames = tr::encode_message(
            {std::uint8_t{99}, tr::MessageKind::specific, std::move(payload)},
            tr::Encryption::encrypted, cipher.get());
        ASSERT_TRUE(frames.has_value());
        const std::lock_guard lock(state->mutex);
        for (auto &frame : frames.value()) {
            state->inbound.push_back(std::move(frame));
        }
        state->cv.notify_all();
    };
    const std::vector<std::byte> config_body{std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                             std::byte{0x01}, std::byte{0x67}, std::byte{0x42}};
    const std::vector<std::byte> idr_body{std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                          std::byte{0x01}, std::byte{0x65}, std::byte{0x88}};
    const std::vector<std::byte> p_body{std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                        std::byte{0x01}, std::byte{0x41}, std::byte{0x10}};
    std::vector<std::byte> config_message{std::byte{0x00}, std::byte{0x01}};
    config_message.insert(config_message.end(), config_body.begin(), config_body.end());
    const std::int64_t t_push_ns = steady_ns();
    push_plain(wiredtest::control_frame(
        aa::protocol::encode(aa::protocol::VersionResponse{1, 6, 0}).value()));
    push_plain(wiredtest::control_frame(
        aa::protocol::encode(aa::protocol::EncapsulatedSsl{{std::byte{1}}}).value()));
    push_plain(wiredtest::discovery_request_frame());
    push_plain(wiredtest::open_request_frame(99));
    push_media(p::encode(p::AvSinkSetup{p::AvSinkCodec::h264}).value());
    push_media(p::encode(p::AvSinkStart{-7, 0}).value());
    push_media(config_message);
    push_media(av_data_message(0x1111, idr_body));
    push_media(av_data_message(0x2222, p_body));
    push_media(av_data_message(0x3333, p_body));
    std::size_t pushed = 0;
    {
        std::lock_guard lock(state->mutex);
        pushed = state->inbound.size();
    }
    {
        std::unique_lock lock(state->mutex);
        (void)state->cv.wait_for(lock, std::chrono::milliseconds{700},
                                 [&] { return state->served >= pushed; });
    }
    auto connected = aa::ipc::connect_video_consumer(
        aa::ipc::VideoSocketPath{(root / "video").string()},
        aa::ipc::VideoClientOptions{{}, aa::ipc::current_uid(), aa::core::Milliseconds{3000}});
    ASSERT_TRUE(connected.has_value()) << aa::to_string(connected.error().code());
    const auto first = connected.value().receive(Milliseconds{3000});
    ASSERT_TRUE(first.has_value()) << "SOLE_INITIAL_CONFIG_IDR_NEVER_REACHED_CONSUMER: "
                                   << aa::to_string(first.error().code());
    ASSERT_EQ(first.value(), aa::ipc::ReceiveStatus::complete);
    const auto &unit = connected.value().access_unit();
    EXPECT_EQ(unit.frame, 1U);
    EXPECT_TRUE(unit.idr);
    std::vector<std::uint8_t> expected;
    expected.reserve(config_body.size() + idr_body.size());
    for (const auto byte : config_body) {
        expected.push_back(std::to_integer<std::uint8_t>(byte));
    }
    for (const auto byte : idr_body) {
        expected.push_back(std::to_integer<std::uint8_t>(byte));
    }
    EXPECT_EQ(unit.payload, expected);
    std::vector<std::uint8_t> p_frame;
    p_frame.reserve(p_body.size());
    for (const auto byte : p_body) {
        p_frame.push_back(std::to_integer<std::uint8_t>(byte));
    }
    const std::int64_t t_recv_ns = steady_ns();
    EXPECT_GE(unit.timestamp_ns, static_cast<std::uint64_t>(t_push_ns));
    EXPECT_LE(unit.timestamp_ns, static_cast<std::uint64_t>(t_recv_ns));
    for (int follow = 0; follow < 2; ++follow) {
        const auto step = connected.value().receive(Milliseconds{3000});
        ASSERT_TRUE(step.has_value());
        ASSERT_EQ(step.value(), aa::ipc::ReceiveStatus::complete);
        EXPECT_FALSE(connected.value().access_unit().idr);
        EXPECT_EQ(connected.value().access_unit().payload, p_frame);
    }
    guard.worker.join();
    const auto output = capture.release();
    admission_link_state.reset();
    EXPECT_EQ(code, 0) << output;
    EXPECT_EQ(runtime_connects.load(), 1) << output;
    const auto ready_at = output.find("\"event\":\"ready\"");
    const auto accepted_at = output.find("\"event\":\"video_accepted\"");
    ASSERT_NE(accepted_at, std::string::npos) << output;
    EXPECT_NE(ready_at, std::string::npos) << output;
    EXPECT_LT(ready_at, accepted_at) << "ready must be infrastructure readiness, pre-gate: "
                                     << output;
    EXPECT_NE(output.find("\"to_state\":\"active\""), std::string::npos) << output;
    std::filesystem::remove_all(root);
}

struct RenderGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    bool fail_write = false;
    bool fail_drain = false;
    bool timed_out = false;
    std::chrono::milliseconds write_timeout{2000};
    std::size_t writes = 0;
    std::size_t drains = 0;
    std::size_t destroyed = 0;
    std::vector<std::vector<std::byte>> pcm;
    std::vector<aa::core::Nanoseconds> timestamps;
    std::vector<std::thread::id> threads;
    std::optional<std::chrono::milliseconds> first_write_delay;
    std::vector<std::int64_t> entered_ns;
    std::vector<std::int64_t> completed_ns;
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
    bool wait_writes(std::size_t count) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds{2}, [&] { return writes >= count; });
    }
};

class GatedRenderSink final : public aa::audio::detail::ObservedAudioSink {
  public:
    explicit GatedRenderSink(std::shared_ptr<RenderGate> gate) : gate_(std::move(gate)) {}
    ~GatedRenderSink() override {
        std::lock_guard lock(gate_->mutex);
        ++gate_->destroyed;
        gate_->threads.push_back(std::this_thread::get_id());
    }
    aa::core::Result<void> write(const aa::audio::MixedChunk &chunk) override {
        std::unique_lock lock(gate_->mutex);
        ++gate_->writes;
        gate_->pcm.emplace_back(chunk.pcm.begin(), chunk.pcm.end());
        gate_->timestamps.push_back(chunk.timestamp);
        gate_->threads.push_back(std::this_thread::get_id());
        gate_->cv.notify_all();
        gate_->entered_ns.push_back(steady_ns());
        if (gate_->first_write_delay && gate_->writes == 1) {
            gate_->cv.wait_until(lock, std::chrono::steady_clock::now() + *gate_->first_write_delay,
                                [&] { return gate_->released; });
            gate_->released = true;
        } else if (!gate_->cv.wait_for(lock, gate_->write_timeout, [&] { return gate_->released; })) {
            gate_->timed_out = true;
            return aa::Error{aa::ErrorCode::timeout};
        }
        if (gate_->fail_write) { return aa::Error{aa::ErrorCode::timeout}; }
        gate_->completed_ns.push_back(steady_ns());
        return {};
    }
    aa::core::Result<void> drain(aa::core::Milliseconds) override {
        std::lock_guard lock(gate_->mutex);
        ++gate_->drains;
        gate_->threads.push_back(std::this_thread::get_id());
        if (gate_->fail_drain) { return aa::Error{aa::ErrorCode::timeout}; }
        return {};
    }
    aa::audio::SinkProbeStats probe_stats() const override { return {}; }
    aa::audio::RenderBarrierStats render_stats() const override { return {}; }
  private:
    std::shared_ptr<RenderGate> gate_;
};

ConcreteAudioRenderWorker::OpenRouter gated_router(const std::shared_ptr<RenderGate> &gate) {
    return [gate](const auto &config) {
        {
            std::lock_guard lock(gate->mutex);
            gate->threads.push_back(std::this_thread::get_id());
        }
        return aa::audio::detail::open_router_with_sink_for_test(
            config, std::make_unique<GatedRenderSink>(gate));
    };
}

TEST(WiredReceiver, WorkerFirstFailureContextFreezesRenderAndSubmitIdentityThroughFinish) {
    ReceiverLogScope logging{g_running};
    static_assert(std::is_trivially_copyable_v<WorkerQueueObservation>);
    static_assert(std::is_trivially_copyable_v<WorkerSuccessfulStep>);
    static_assert(std::is_trivially_copyable_v<WorkerAdmission>);
    static_assert(std::is_trivially_copyable_v<WorkerIdleObservation>);
    static_assert(std::is_trivially_copyable_v<WorkerFailureObservation>);
    std::cout << "WORKER_RECORD_BYTES queue=" << sizeof(WorkerQueueObservation)
              << " success=" << sizeof(WorkerSuccessfulStep)
              << " admission=" << sizeof(WorkerAdmission)
              << " idle=" << sizeof(WorkerIdleObservation)
              << " failure=" << sizeof(WorkerFailureObservation) << '\n';
    for (const bool negative : {false, true}) {
        SCOPED_TRACE(negative);
        auto gate = std::make_shared<RenderGate>();
        ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate)};
        ASSERT_TRUE(audio.ready());
        const auto role = aa::protocol::ChannelRole::media_audio;
        const std::vector<std::byte> pcm(960, std::byte{1});
        ASSERT_TRUE(audio.submit({{role, 1, {1'000'000'000}}, pcm}, {role, 0, 11, 7}));
        ASSERT_TRUE(gate->wait_writes(1));
        ASSERT_TRUE(audio.submit({{role, 2, {negative ? -1LL : 0LL}}, pcm}, {role, 0, 12, 7}));
        gate->release();
        ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        ASSERT_TRUE(audio.failure());
        EXPECT_EQ(audio.failure()->code(), aa::ErrorCode::invalid_argument);
        const auto frozen = audio.failure_context();
        ASSERT_TRUE(frozen);
        EXPECT_GT(frozen->latched_at.count, 0);
        EXPECT_EQ(frozen->origin, negative ? AudioFailureOrigin::router_submit : AudioFailureOrigin::render_step);
        ASSERT_TRUE(frozen->parent);
        EXPECT_EQ(frozen->parent->delivery, (aa::channels::AvDeliveryId{role, 0, 12, 7}));
        EXPECT_EQ(frozen->parent->message_sequence, 2U);
        EXPECT_EQ(frozen->parent->original_scheduled.count, negative ? -1 : 0);
        ASSERT_TRUE(frozen->worker.admission);
        EXPECT_EQ(frozen->worker.admission->parent.delivery, frozen->parent->delivery);
        EXPECT_EQ(frozen->worker.admission->disposition, IdleDisposition::absent);
        EXPECT_TRUE(frozen->worker.admission->queue.in_flight);
        EXPECT_EQ(frozen->worker.admission->queue.queued_by_role[0], 1U);
        EXPECT_EQ(frozen->worker.queue.queued_by_role[0], 1U);
        EXPECT_FALSE(frozen->worker.queue.in_flight);
        ASSERT_TRUE(frozen->worker.last_completion);
        ASSERT_TRUE(frozen->worker.last_success);
        EXPECT_LE(frozen->worker.last_completion->completed, frozen->worker.last_success->returned_at);
        EXPECT_LE(frozen->worker.last_success->returned_at, frozen->worker.last_success->reconciled_at);
        if (negative) {
            EXPECT_FALSE(frozen->router);
        } else {
            ASSERT_TRUE(frozen->router);
            EXPECT_EQ(frozen->router->reason, aa::audio::RenderFailureReason::late);
            EXPECT_EQ(frozen->router->entry_cursor, 240U);
            EXPECT_EQ(frozen->router->intended, -48000);
        }
        const auto json = audio_failure_json(*frozen);
        if (!negative) {
            EXPECT_NE(json.find("\"start\":null"), std::string::npos);
            EXPECT_NE(json.find("\"raw_gap\":null"), std::string::npos);
            EXPECT_NE(json.find("\"now_ns\":null"), std::string::npos);
            EXPECT_NE(json.find("\"effective_backpressure\":null"), std::string::npos);
            EXPECT_NE(json.find("\"effective_idle\":null"), std::string::npos);
        }
        const auto event = audio_failure_event_json("audio_sink_failure", audio.failure()->code(), frozen);
        EXPECT_NE(event.find("\"audio_failure\":" + json), std::string::npos);
        audio.invalidate(role, 1);
        EXPECT_FALSE(audio.submit({{role, 99, {0}}, pcm}, {role, 1, 99, 7}));
        ASSERT_FALSE(audio.finish());
        ASSERT_TRUE(audio.failure_context());
        EXPECT_EQ(audio_failure_json(*audio.failure_context()), json);
        EXPECT_EQ(audio_failure_json(*frozen), json);
        const auto finish = audio_failure_event_json("audio_finish", audio.failure()->code(), audio.failure_context());
        EXPECT_NE(finish.find("\"audio_failure\":" + json), std::string::npos);
        EXPECT_EQ(gate->writes, 1U);
        EXPECT_FALSE(gate->timed_out);
    }
}

TEST(WiredReceiver, WorkerSinkTimeoutContextPreservesUnderlyingErrorAndDetachedInterval) {
    ReceiverLogScope logging{g_running};
    auto gate = std::make_shared<RenderGate>();
    gate->fail_write = true;
    ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate)};
    ASSERT_TRUE(audio.ready());
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(960, std::byte{1});
    ASSERT_TRUE(audio.submit({{role, 1, {1'000'000'000}}, pcm}, {role, 0, 1, 7}));
    ASSERT_TRUE(gate->wait_writes(1));
    audio.invalidate(role, 1);
    gate->release();
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(audio.failure());
    EXPECT_EQ(audio.failure()->code(), aa::ErrorCode::audio_sink_unavailable);
    const auto frozen = audio.failure_context();
    ASSERT_TRUE(frozen);
    EXPECT_EQ(frozen->origin, AudioFailureOrigin::render_step);
    EXPECT_FALSE(frozen->parent);
    ASSERT_TRUE(frozen->router);
    EXPECT_EQ(frozen->router->reason, aa::audio::RenderFailureReason::sink_write_error);
    EXPECT_EQ(frozen->router->underlying_code, aa::ErrorCode::timeout);
    EXPECT_EQ(frozen->router->output_start, 0U);
    EXPECT_EQ(frozen->router->output_end, 240U);
    EXPECT_FALSE(frozen->router->offending);
    const auto json = audio_failure_json(*frozen);
    EXPECT_NE(json.find("\"underlying_code\":\"timeout\""), std::string::npos);
    ASSERT_FALSE(audio.finish());
    ASSERT_TRUE(audio.failure_context());
    EXPECT_EQ(audio_failure_json(*audio.failure_context()), json);
    EXPECT_EQ(gate->writes, 1U);
}

TEST(WiredReceiver, WorkerSubmitFailureNeverAttachesStaleRouterRenderSnapshot) {
    ReceiverLogScope logging{g_running};
    auto gate = std::make_shared<RenderGate>();
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(960, std::byte{1});
    ConcreteAudioRenderWorker audio{pcm_router_config(), [&](const auto& config) {
        auto opened = gated_router(gate)(config);
        if (opened) {
            EXPECT_TRUE(opened.value().submit({{role, 1, {1'000'000'000}}, pcm}));
            EXPECT_TRUE(opened.value().submit({{role, 2, {1'300'000'000}}, pcm}));
            EXPECT_FALSE(opened.value().render_step());
            EXPECT_TRUE(opened.value().report().render_failure);
        }
        return opened;
    }};
    ASSERT_TRUE(audio.ready());
    ASSERT_TRUE(audio.submit({{role, 3, {-1}}, pcm}, {role, 0, 13, 7}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    const auto frozen = audio.failure_context();
    ASSERT_TRUE(frozen);
    EXPECT_EQ(frozen->origin, AudioFailureOrigin::router_submit);
    EXPECT_FALSE(frozen->router);
    ASSERT_TRUE(frozen->parent);
    EXPECT_EQ(frozen->parent->message_sequence, 3U);
    ASSERT_TRUE(frozen->worker.admission);
    EXPECT_EQ(frozen->worker.admission->parent.delivery.sequence, 13U);
    EXPECT_EQ(frozen->worker.admission->disposition, IdleDisposition::absent);
    EXPECT_FALSE(frozen->worker.last_completion);
    EXPECT_FALSE(frozen->worker.last_success);
    EXPECT_FALSE(frozen->worker.last_idle_close);
    const auto json = audio_failure_json(*frozen);
    ASSERT_FALSE(audio.finish());
    ASSERT_TRUE(audio.failure_context());
    EXPECT_EQ(audio_failure_json(*audio.failure_context()), json);
    EXPECT_EQ(gate->writes, 0U);
    ASSERT_TRUE(audio.final_report()->render_failure);
    EXPECT_EQ(audio.final_report()->render_failure->reason, aa::audio::RenderFailureReason::single_gap);
}

TEST(WiredReceiver, WorkerRenderFailureMatchesFullHeaderAndOmitsAmbiguousDispatch) {
    ReceiverLogScope logging{g_running};
    for (const bool ambiguous : {false, true}) {
        SCOPED_TRACE(ambiguous);
        auto gate = std::make_shared<RenderGate>();
        ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate)};
        ASSERT_TRUE(audio.ready());
        const auto role = aa::protocol::ChannelRole::media_audio;
        const std::vector<std::byte> pcm(960, std::byte{1});
        ASSERT_TRUE(audio.submit({{role, 1, {1'000'000'000}}, pcm}, {role, 0, 1, 7}));
        ASSERT_TRUE(gate->wait_writes(1));
        ASSERT_TRUE(audio.submit({{role, 2, {ambiguous ? 0LL : 1'005'000'000LL}}, pcm}, {role, 0, 2, 7}));
        ASSERT_TRUE(audio.submit({{role, 2, {0}}, pcm}, {role, 0, 3, 7}));
        gate->release();
        ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        const auto frozen = audio.failure_context();
        ASSERT_TRUE(frozen);
        ASSERT_TRUE(frozen->router);
        EXPECT_EQ(frozen->router->reason, aa::audio::RenderFailureReason::late);
        if (ambiguous) {
            EXPECT_FALSE(frozen->parent);
            EXPECT_FALSE(frozen->worker.admission);
            EXPECT_NE(audio_failure_json(*frozen).find("\"admission\":null"), std::string::npos);
        }
        else {
            ASSERT_TRUE(frozen->parent);
            EXPECT_EQ(frozen->parent->delivery.sequence, 3U);
            EXPECT_EQ(frozen->parent->original_scheduled.count, 0);
            ASSERT_TRUE(frozen->worker.admission);
            EXPECT_EQ(frozen->worker.admission->parent.delivery.sequence, 3U);
            EXPECT_EQ(frozen->worker.admission->parent.original_scheduled.count, 0);
        }
        const auto json = audio_failure_json(*frozen);
        ASSERT_FALSE(audio.finish());
        ASSERT_TRUE(audio.failure_context());
        EXPECT_EQ(audio_failure_json(*audio.failure_context()), json);
        EXPECT_EQ(gate->writes, 1U);
    }
}

TEST(WiredReceiver, ActualMainIdleAudioCompletesAndRenderFailureNeverReconnects) {
    for (const bool fail_write : {false, true}) {
        auto gate = std::make_shared<RenderGate>();
        gate->fail_write = fail_write;
        runtime_send_audio = true;
        runtime_audio_ack_attempts.store(0);
        runtime_polls.store(0);
        runtime_sink_override = [gate] { return std::make_unique<GatedRenderSink>(gate); };
        runtime_on_poll = [gate] { gate->release(); };
        const auto result = run_runtime(false, 200, true);
        runtime_sink_override = {};
        runtime_on_poll = {};
        runtime_send_audio = false;
        EXPECT_EQ(result.code, fail_write ? 1 : 0) << result.output;
        EXPECT_EQ(runtime_connects.load(), 1) << result.output;
        if (fail_write) {
            // ACK-after-admission contract: the admission ACK may already be
            // on the wire when the render fails; the typed session failure
            // below is the invariant, not the ACK count.
            EXPECT_LE(runtime_audio_ack_attempts.load(), 1) << result.output;
        } else {
            EXPECT_EQ(runtime_audio_ack_attempts.load(), 1) << result.output;
        }
        EXPECT_GT(runtime_polls.load(), 0);
        EXPECT_EQ(gate->writes, 1U);
        EXPECT_FALSE(gate->timed_out);
        EXPECT_EQ(gate->drains, 1U);
        EXPECT_EQ(gate->destroyed, 1U);
        EXPECT_EQ(result.output.find("\"to_state\":\"reconnecting\""), std::string::npos);
        if (fail_write) {
            EXPECT_NE(result.output.find("audio_sink_unavailable"), std::string::npos);
            EXPECT_EQ(result.output.find("audio_chunks"), std::string::npos);
        }
    }
}

TEST(WiredReceiver, ActualMainAckFailureUsesTransportRecoveryWithoutRetryingAudio) {
    auto gate = std::make_shared<RenderGate>();
    runtime_send_audio = true;
    runtime_ack_send_failure = true;
    runtime_audio_ack_attempts.store(0);
    runtime_sink_override = [gate] { return std::make_unique<GatedRenderSink>(gate); };
    // ACK-after-admission contract: the ACK-send failure surfaces right after
    // dispatch and the owner enters transport recovery while the render is
    // still in flight - the render must not depend on the owner's poll loop.
    gate->release();
    const auto result = run_runtime(false, 200, true);
    runtime_sink_override = {};
    runtime_on_poll = {};
    runtime_send_audio = false;
    runtime_ack_send_failure = false;
    EXPECT_EQ(result.code, 0) << result.output;
    EXPECT_GT(runtime_connects.load(), 1) << result.output;
    EXPECT_EQ(runtime_audio_ack_attempts.load(), 1);
    EXPECT_LE(gate->writes, 4U);
    EXPECT_EQ(gate->drains, 1U);
    EXPECT_FALSE(gate->timed_out);
    EXPECT_NE(result.output.find("\"to_state\":\"reconnecting\""), std::string::npos);
    EXPECT_NE(result.output.find("\"code\":\"transport_io\""), std::string::npos);
}

TEST(WiredReceiver, ActualMainPropagatesFinalDrainFailureAndJoinsEarlyOpenFailure) {
    auto gate = std::make_shared<RenderGate>();
    gate->released = true;
    gate->fail_drain = true;
    runtime_sink_override = [gate] { return std::make_unique<GatedRenderSink>(gate); };
    const auto drained = run_runtime(false, 100, true);
    runtime_sink_override = {};
    EXPECT_EQ(drained.code, 1) << drained.output;
    EXPECT_NE(drained.output.find("\"detail\":\"audio_finish\""), std::string::npos);
    EXPECT_EQ(gate->drains, 1U);
    EXPECT_EQ(gate->destroyed, 1U);
    runtime_sink_override = [] { return std::unique_ptr<aa::audio::detail::ObservedAudioSink>{}; };
    const auto opened = run_runtime(false, 100, true);
    runtime_sink_override = {};
    EXPECT_EQ(opened.code, 3) << opened.output;
    EXPECT_EQ(runtime_connects.load(), 0);
    EXPECT_NE(opened.output.find("\"detail\":\"audio_router\""), std::string::npos);
}

TEST(WiredReceiver, ActualMainPollReportsWorkerOversizeOriginWithoutChangingError) {
    runtime_audio_ack_attempts.store(0);
    runtime_send_audio = true;
    runtime_audio_bytes = 38404;
    const auto result = run_runtime(false, 500, true);
    runtime_audio_bytes = 0;
    runtime_send_audio = false;
    EXPECT_EQ(result.code, 1) << result.output;
    EXPECT_EQ(runtime_audio_ack_attempts.load(), 0);
    EXPECT_NE(result.output.find("\"event\":\"err\",\"detail\":\"poll\",\"code\":\"protocol_payload_oversize\""),
              std::string::npos) << result.output;
    EXPECT_EQ(result.output.find("\"queue_rejection\":"), std::string::npos) << result.output;
}

TEST(WiredReceiver, ActualMainTransientStallIndependentCadenceAndWorstCaseBurst) {
    for (const int stall_ms : {0, 50, 100, 150, 200, 250, 300, -50}) {
        SCOPED_TRACE(stall_ms);
        CadencedPcmSource source;
        source.burst = stall_ms < 0; // Deliberately ACK-paced worst-case, NOT normal PCM cadence.
        // 8 chunks put the last arrival only ~200 ms behind the cursor (step-size
        // dependent); 16 exceed the cap unambiguously.
        if (source.burst) { source.chunks = 16; }
        auto gate = std::make_shared<RenderGate>();
        gate->first_write_delay = std::chrono::milliseconds{std::abs(stall_ms)};
        runtime_send_audio = true;
        runtime_pcm_source = &source;
        runtime_audio_ack_attempts.store(0);
        runtime_sink_override = [gate] { return std::make_unique<GatedRenderSink>(gate); };
        const auto result = run_runtime(false, 450, true);
        runtime_sink_override = {};
        runtime_pcm_source = nullptr;
        runtime_send_audio = false;
        // Third queued chunk of one role arrives before the stalled first
        // write returns: its credit is withheld instead of over-admitting.
        const bool deferred = std::abs(stall_ms) >= 100 || source.burst;
        EXPECT_FALSE(gate->timed_out);
        EXPECT_EQ(runtime_connects.load(), 1);
        EXPECT_EQ(result.output.find("ipc_queue_full"), std::string::npos) << result.output;
        EXPECT_EQ(result.output.find("\"event\":\"audio_credit_deferred\"") != std::string::npos,
                  deferred) << result.output;
        if (source.burst) {
            // Credit pacing holds the queue, but an unpaced source still runs
            // >200 ms ahead of the cursor: the router's unchanged late-arrival
            // cap, not queue overflow, ends this deliberately unrealistic case.
            EXPECT_EQ(result.code, 1) << result.output;
            EXPECT_NE(result.output.find("\"detail\":\"audio_sink_failure\",\"code\":\"invalid_argument\""),
                      std::string::npos) << result.output;
            std::cout << "STALL_RESULT mode=worst_case_burst stall_ms=" << std::abs(stall_ms)
                      << " exit=" << result.code << " arrivals=" << source.arrivals.size()
                      << " acks=" << source.acks.size() << '\n' << result.output;
            continue;
        }
        EXPECT_EQ(result.code, 0) << result.output;
        ASSERT_EQ(source.arrivals.size(), 8U) << result.output;
        ASSERT_EQ(source.acks.size(), 8U) << result.output;
        EXPECT_EQ(runtime_audio_ack_attempts.load(), static_cast<int>(source.acks.size()));
        EXPECT_EQ(result.output.find("\"to_state\":\"reconnecting\""), std::string::npos);
        ASSERT_FALSE(gate->completed_ns.empty()) << result.output;
        EXPECT_EQ(gate->drains, 1U);
        EXPECT_EQ(gate->destroyed, 1U);
        EXPECT_EQ(result.output.find("\"event\":\"err\""), std::string::npos) << result.output;
        std::cout << "STALL_RESULT mode=independent_pcm"
                  << " stall_ms=" << std::abs(stall_ms) << " exit=" << result.code
                  << " held_chunks=" << source.held_chunks.size() << '\n';
        for (std::size_t i = 0; i < source.arrivals.size(); ++i) {
            std::cout << "STALL_ARRIVAL chunk=" << i + 1 << " arrival_ns=" << source.arrivals[i];
            if (i < source.acks.size()) { std::cout << " ack_ns=" << source.acks[i]; }
            std::cout << '\n';
        }
        const auto epoch_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            source.epoch.time_since_epoch()).count();
        for (std::size_t i = 0; i < source.arrivals.size(); ++i) {
            EXPECT_GE(source.arrivals[i], static_cast<std::int64_t>(i) * 8192 * 1'000'000'000LL / 192000);
            EXPECT_GE(source.acks[i], source.arrivals[i]);
            if (i + 1 < source.arrivals.size()) { EXPECT_LE(source.acks[i], source.arrivals[i + 1]); }
        }
        EXPECT_EQ(!source.held_chunks.empty(), stall_ms >= 150); // Chunk 4 is due at 128 ms.
        if (deferred) {
            EXPECT_GE(source.acks[2], gate->completed_ns.front() - epoch_ns);
        } else {
            EXPECT_LT(source.acks[2] - source.arrivals[2], 20'000'000);
        }
        for (std::size_t i = 0; i < gate->completed_ns.size(); ++i) {
            std::cout << "STALL_RENDER write=" << i + 1
                      << " entered_ns=" << gate->entered_ns[i] - epoch_ns
                      << " completed_ns=" << gate->completed_ns[i] - epoch_ns << '\n';
        }
        std::cout << result.output;
    }
}

class ReceiverPollTransport final : public aa::transport::Transport {
  public:
    wiredtest::ScriptedTransport wire;
    std::thread::id owner = std::this_thread::get_id();
    std::size_t queued = 0;
    std::size_t polls = 0;
    bool wrong_thread = false;
    aa::core::Deadline::Clock::time_point actual_deadline{};
    aa::core::Deadline::Clock::time_point service_deadline{};
    aa::transport::Kind kind() const noexcept override { return wire.kind(); }
    void set_io_deadline(aa::core::Deadline::Clock::time_point value) noexcept override {
        actual_deadline = value;
    }
    aa::core::Result<void> open(aa::core::CancellationToken token) override {
        wrong_thread |= std::this_thread::get_id() != owner;
        return wire.open(token);
    }
    aa::core::Result<void> send(std::span<const std::byte> frame) override {
        wrong_thread |= std::this_thread::get_id() != owner;
        return wire.send(frame);
    }
    aa::core::Result<std::vector<std::byte>> receive() override {
        wrong_thread |= std::this_thread::get_id() != owner;
        if (queued != 0) { --queued; }
        return wire.receive();
    }
    aa::core::Result<std::optional<std::vector<std::byte>>>
    poll_receive(aa::core::Deadline::Clock::time_point until) override {
        ++polls;
        service_deadline = until;
        if (queued != 0) {
            auto frame = receive();
            if (!frame) { return frame.error(); }
            return std::optional{std::move(frame).value()};
        }
        std::mutex mutex;
        std::condition_variable cv;
        std::unique_lock lock(mutex);
        cv.wait_until(lock, until);
        return std::optional<std::vector<std::byte>>{};
    }
    void push(std::vector<std::byte> frame) {
        ++queued;
        wire.push_inbound(std::move(frame));
    }
    void close() noexcept override { wire.close(); }
};

class ReceiverAudioDispatch : public testing::Test {
  protected:
    ReceiverLogScope logging{g_running};
    aa::session::test::MapTrustStore trust;
    aa::session::test::ManualClock clock;
    wiredtest::CaptureSink capture;
    aa::diagnostics::Diagnostics diagnostics{capture};
    std::shared_ptr<RenderGate> gate = std::make_shared<RenderGate>();
    ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate)};
    ReceiverPollTransport transport;
    VideoOutlet video{log_line};
    aa::app::WiredSession session{{trust, clock, diagnostics}};
    wiredtest::FakeHandshakeCipher *cipher = nullptr;
    std::optional<aa::ipc::VideoConsumer> consumer;
    std::int64_t base = steady_ns();
    void SetUp() override {
        clock.set({base});
        const aa::trust::PhoneIdentity phone{"synthetic-audio-dispatch"};
        trust.set(phone, aa::trust::Decision::approved);
        ASSERT_TRUE(audio.ready());
        ASSERT_TRUE(session.configure(service_descriptors(
            aa::input::CapabilityDescriptor::bench_defaults())));
        ASSERT_TRUE(register_av_sinks(session, video, audio));
        session.attach_transport(&transport);
        auto owned = std::make_unique<wiredtest::FakeHandshakeCipher>();
        cipher = owned.get();
        session.attach_cipher(std::move(owned));
        ASSERT_TRUE(session.apply(aa::session::Event::start_discovery));
        ASSERT_TRUE(session.phone_discovered(phone));
        ASSERT_TRUE(session.apply(aa::session::Event::transport_ready));
        transport.push(wiredtest::control_frame(
            aa::protocol::encode(aa::protocol::VersionResponse{1, 6, 0}).value()));
        ASSERT_TRUE(session.poll_transport());
        transport.push(wiredtest::control_frame(
            aa::protocol::encode(aa::protocol::EncapsulatedSsl{{std::byte{1}}}).value()));
        ASSERT_TRUE(session.poll_transport());
        ASSERT_TRUE(session.authenticated());
        ASSERT_TRUE(session.announce_services());
        auto pair = videotest::make_pair({});
        ASSERT_TRUE(pair);
        video.install(std::move(pair.value().producer));
        consumer.emplace(std::move(pair.value().consumer));
        ASSERT_TRUE(receive(100, aa::protocol::encode(
            aa::protocol::AvSinkSetup{aa::protocol::AvSinkCodec::pcm}).value()));
        ASSERT_TRUE(receive(100, aa::protocol::encode(aa::protocol::AvSinkStart{-17, 0}).value()));
        ASSERT_TRUE(receive(99, aa::protocol::encode(
            aa::protocol::AvSinkSetup{aa::protocol::AvSinkCodec::h264}).value()));
        ASSERT_TRUE(receive(99, aa::protocol::encode(aa::protocol::AvSinkStart{-7, 0}).value()));
        ASSERT_TRUE(session.apply(aa::session::Event::negotiation_succeeded));
    }
    void TearDown() override {
        gate->release();
        (void)audio.finish();
        EXPECT_FALSE(gate->timed_out);
        EXPECT_FALSE(transport.wrong_thread);
    }
    aa::core::Result<bool> receive(std::uint8_t channel, std::vector<std::byte> payload) {
        auto frames = aa::transport::encode_message(
            {channel, aa::transport::MessageKind::specific, std::move(payload)},
            aa::transport::Encryption::encrypted, cipher);
        if (!frames) { return frames.error(); }
        aa::core::Result<bool> result{false};
        for (auto &frame : frames.value()) {
            transport.push(std::move(frame));
            result = session.poll_transport();
            if (!result) { return result.error(); }
        }
        return result;
    }
    aa::core::Result<bool> start_audio_channel(std::uint8_t channel) {
        if (auto setup = receive(channel, aa::protocol::encode(aa::protocol::AvSinkSetup{}).value());
            !setup) {
            return setup;
        }
        return receive(channel, aa::protocol::encode(aa::protocol::AvSinkStart{7, 0}).value());
    }
    aa::core::Result<void> wait_owner() {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (audio.outstanding() && std::chrono::steady_clock::now() < until) {
            auto result = receive_with_audio(session, audio);
            if (!result) { return result.error(); }
        }
        if (audio.outstanding()) { return aa::Error{aa::ErrorCode::timeout}; }
        return service_audio_completions(session, audio);
    }
};

TEST_F(ReceiverAudioDispatch, BlockedRenderOwnerAcksAdmissionBeforeRenderWhileVideoAndPingStillDispatch) {
    auto pcm = wiredtest::pcm_unit();
    const auto original = pcm;
    const auto before = transport.wire.sent_count();
    ASSERT_TRUE(receive(100, av_data_message(123456, pcm)));
    pcm.assign(pcm.size(), std::byte{0});
    ASSERT_TRUE(gate->wait_writes(1));
    EXPECT_EQ(transport.wire.sent_count(), before);
    EXPECT_TRUE(audio.outstanding());
    // ACK-after-admission contract: the owner credits the admitted delivery
    // while the render is still blocked (no write completed yet) and the PCM
    // reservation stays charged until the render resolves.
    ASSERT_TRUE(service_audio_completions(session, audio));
    EXPECT_EQ(transport.wire.sent_count(), before + 1);
    const auto admitted = aa::transport::decode_frame(transport.wire.last_sent());
    ASSERT_TRUE(admitted);
    EXPECT_EQ(admitted.value().header.channel, 100);
    EXPECT_EQ(admitted.value().header.encryption, aa::transport::Encryption::encrypted);
    EXPECT_EQ(cipher->open(admitted.value().payload).value(),
              aa::protocol::encode_av_sink_ack(-17).value());
    EXPECT_TRUE(audio.outstanding());
    ASSERT_TRUE(receive(99, av_data_message(654321, wiredtest::kVideoIdr)));
    ASSERT_TRUE(consumer->receive(Milliseconds{500}));
    EXPECT_EQ(consumer->access_unit().timestamp_ns, static_cast<std::uint64_t>(base));
    ASSERT_TRUE(receive(0, aa::protocol::encode(aa::protocol::PingRequest{42, {}}).value()));
    EXPECT_EQ(session.pings_answered(), 1U);
    EXPECT_EQ(transport.wire.sent_count(), before + 3);
    const auto yielded = receive_with_audio(session, audio);
    ASSERT_TRUE(yielded);
    EXPECT_FALSE(yielded.value());
    EXPECT_LT(transport.service_deadline, transport.actual_deadline);
    EXPECT_FALSE(session.failure());
    EXPECT_EQ(transport.wire.sent_count(), before + 3);
    gate->release();
    ASSERT_TRUE(wait_owner());
    EXPECT_EQ(transport.wire.sent_count(), before + 3);
    ASSERT_TRUE(service_audio_completions(session, audio));
    EXPECT_EQ(transport.wire.sent_count(), before + 3);
    EXPECT_GT(transport.polls, 0U);
    ASSERT_TRUE(audio.finish());
    std::vector<std::byte> rendered;
    for (const auto &chunk : gate->pcm) { rendered.insert(rendered.end(), chunk.begin(), chunk.end()); }
    EXPECT_EQ(rendered, original);
    EXPECT_EQ(gate->timestamps.front().count, base);
    EXPECT_EQ(gate->drains, 1U);
    EXPECT_EQ(gate->destroyed, 1U);
    for (const auto thread : gate->threads) {
        EXPECT_NE(thread, transport.owner);
        EXPECT_EQ(thread, gate->threads.front());
    }
}

TEST_F(ReceiverAudioDispatch, AckSendFailureConsumesPendingWithoutRenderOrAckRetry) {
    ASSERT_TRUE(receive(100, av_data_message(1, wiredtest::pcm_unit())));
    ASSERT_TRUE(gate->wait_writes(1));
    const auto before = transport.wire.sent_count();
    transport.wire.fail_send_at(before);
    gate->release();
    const auto result = wait_owner();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::transport_io);
    EXPECT_FALSE(audio.failure());
    transport.wire.fail_send_at(std::nullopt);
    ASSERT_TRUE(service_audio_completions(session, audio));
    EXPECT_EQ(transport.wire.sent_count(), before);
    EXPECT_LE(gate->writes, 4U);
    EXPECT_EQ(gate->timestamps.front().count, base);
    EXPECT_FALSE(session.failure());
}

TEST_F(ReceiverAudioDispatch, StopSameIdStartRestartAndReconnectDoNotAckStaleRender) {
    ASSERT_TRUE(receive(100, av_data_message(1, wiredtest::pcm_unit())));
    ASSERT_TRUE(gate->wait_writes(1));
    ASSERT_TRUE(receive(100, aa::protocol::encode(aa::protocol::AvSinkStart{-17, 0}).value()));
    const auto duplicate = receive(100, av_data_message(2, wiredtest::pcm_unit()));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), aa::ErrorCode::ipc_queue_full);
    ASSERT_TRUE(receive(100, aa::protocol::encode(aa::protocol::AvSinkStop{}).value()));
    ASSERT_TRUE(receive(100, aa::protocol::encode(aa::protocol::AvSinkStart{-17, 0}).value()));
    clock.set({base + 20'000'000});
    ASSERT_TRUE(receive(100, av_data_message(3, wiredtest::pcm_unit())));
    ASSERT_TRUE(receive(100, aa::protocol::encode(aa::protocol::AvSinkStart{-18, 0}).value()));
    session.attach_transport(&transport);
    const auto before = transport.wire.sent_count();
    EXPECT_EQ(gate->drains, 0U);
    gate->release();
    ASSERT_TRUE(wait_owner());
    EXPECT_EQ(transport.wire.sent_count(), before);
    ASSERT_TRUE(audio.finish());
    EXPECT_EQ(gate->writes, 1U);
    EXPECT_EQ(gate->drains, 1U);
}

TEST_F(ReceiverAudioDispatch, StaleRenderFailureStillFailsOwnerWithoutAckOrRecoveryClassification) {
    {
        std::lock_guard lock(gate->mutex);
        gate->fail_write = true;
    }
    ASSERT_TRUE(receive(100, av_data_message(1, wiredtest::pcm_unit())));
    ASSERT_TRUE(gate->wait_writes(1));
    ASSERT_TRUE(receive(100, aa::protocol::encode(aa::protocol::AvSinkStop{}).value()));
    session.attach_transport(&transport);
    const auto before = transport.wire.sent_count();
    gate->release();
    const auto result = wait_owner();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::audio_sink_unavailable);
    ASSERT_TRUE(session.failure());
    EXPECT_EQ(session.failure()->code(), aa::ErrorCode::audio_sink_unavailable);
    EXPECT_EQ(transport.wire.sent_count(), before);
    EXPECT_FALSE(audio.finish());
    EXPECT_FALSE(audio.finish());
    EXPECT_EQ(gate->writes, 1U);
    EXPECT_EQ(gate->drains, 1U);
}

TEST_F(ReceiverAudioDispatch, AdmissionAckThenRenderFailureStillFailsSessionTyped) {
    {
        std::lock_guard lock(gate->mutex);
        gate->fail_write = true;
    }
    ASSERT_TRUE(receive(100, av_data_message(1, wiredtest::pcm_unit())));
    ASSERT_TRUE(gate->wait_writes(1));
    const auto before = transport.wire.sent_count();
    ASSERT_TRUE(service_audio_completions(session, audio));
    EXPECT_EQ(transport.wire.sent_count(), before + 1);
    const auto acked = aa::transport::decode_frame(transport.wire.last_sent());
    ASSERT_TRUE(acked);
    EXPECT_EQ(acked.value().header.channel, 100);
    EXPECT_EQ(cipher->open(acked.value().payload).value(),
              aa::protocol::encode_av_sink_ack(-17).value());
    gate->release();
    const auto result = wait_owner();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::audio_sink_unavailable);
    ASSERT_TRUE(session.failure());
    EXPECT_EQ(session.failure()->code(), aa::ErrorCode::audio_sink_unavailable);
    EXPECT_EQ(transport.wire.sent_count(), before + 1);
    EXPECT_FALSE(audio.finish());
}

TEST_F(ReceiverAudioDispatch, StopAndRestartKeepsRouterTimelineAndAcksOnlyNewSession) {
    ASSERT_TRUE(receive(100, av_data_message(1, wiredtest::pcm_unit())));
    ASSERT_TRUE(gate->wait_writes(1));
    ASSERT_TRUE(receive(100, aa::protocol::encode(aa::protocol::AvSinkStop{}).value()));
    const auto before = transport.wire.sent_count();
    gate->release();
    ASSERT_TRUE(wait_owner());
    EXPECT_EQ(transport.wire.sent_count(), before);
    EXPECT_EQ(gate->drains, 0U);
    ASSERT_TRUE(receive(100, aa::protocol::encode(aa::protocol::AvSinkStart{-18, 0}).value()));
    clock.set({base + 20'000'000});
    ASSERT_TRUE(receive(100, av_data_message(999, wiredtest::pcm_unit())));
    ASSERT_TRUE(wait_owner());
    EXPECT_EQ(transport.wire.sent_count(), before + 1);
    const auto frame = aa::transport::decode_frame(transport.wire.last_sent());
    ASSERT_TRUE(frame);
    EXPECT_EQ(cipher->open(frame.value().payload).value(), aa::protocol::encode_av_sink_ack(-18).value());
    ASSERT_TRUE(audio.finish());
    const auto new_start = std::find_if(gate->timestamps.begin(), gate->timestamps.end(), [&](const auto &stamp) {
        return stamp.count == base + 20'000'000;
    });
    EXPECT_NE(new_start, gate->timestamps.end());
    EXPECT_EQ(gate->timestamps.back().count, base + 20'000'000);
    EXPECT_EQ(gate->drains, 1U);
}

TEST_F(ReceiverAudioDispatch, ConformingRestartFirstChunkIsAdmittedBehindHeldGuidance) {
    const std::vector<std::byte> guidance(2048, std::byte{1});
    ASSERT_TRUE(start_audio_channel(101));
    // A conforming phone sends guidance only while credited (first message per
    // start is uncredited), with output held so every admitted job stays queued.
    std::size_t admitted = 0;
    for (; admitted < 8; ++admitted) {
        const auto acks = transport.wire.sent_count();
        ASSERT_TRUE(receive(101, av_data_message(admitted + 1, guidance)));
        if (admitted == 0) { ASSERT_TRUE(gate->wait_writes(1)); }
        ASSERT_TRUE(service_audio_completions(session, audio));
        if (transport.wire.sent_count() == acks) { ++admitted; break; }
    }
    ASSERT_EQ(admitted, 1U);
    // Live arm B: media restarts while guidance is held; its first chunk is
    // uncredited by protocol, so the receiver must have room for it.
    ASSERT_TRUE(receive(100, aa::protocol::encode(aa::protocol::AvSinkStop{}).value()));
    ASSERT_TRUE(receive(100, aa::protocol::encode(aa::protocol::AvSinkStart{-18, 0}).value()));
    const auto media = receive(100, av_data_message(1, std::vector<std::byte>(8192, std::byte{2})));
    ASSERT_TRUE(media) << aa::to_string(media.error().code());
    EXPECT_FALSE(session.failure());
    gate->release();
    ASSERT_TRUE(wait_owner());
}

TEST_F(ReceiverAudioDispatch, EveryGrantedGuidanceCreditFitsWithHeldMedia) {
    const std::vector<std::byte> media(8192, std::byte{2});
    for (std::uint64_t i = 0; i < 2; ++i) {
        ASSERT_TRUE(receive(100, av_data_message(i + 1, media)));
        if (i == 0) { ASSERT_TRUE(gate->wait_writes(1)); }
        ASSERT_TRUE(service_audio_completions(session, audio));
    }
    ASSERT_TRUE(start_audio_channel(101));
    const std::vector<std::byte> guidance(2048, std::byte{1});
    for (std::uint64_t i = 0; i < 8; ++i) {
        const auto acks = transport.wire.sent_count();
        const auto result = receive(101, av_data_message(i + 1, guidance));
        ASSERT_TRUE(result) << aa::to_string(result.error().code());
        ASSERT_TRUE(service_audio_completions(session, audio));
        if (transport.wire.sent_count() == acks) { break; }
    }
    EXPECT_FALSE(session.failure());
    gate->release();
    ASSERT_TRUE(wait_owner());
}

TEST_F(ReceiverAudioDispatch, SmallerMediaCreditReservesMaximumNextEnvelope) {
    const auto before = transport.wire.sent_count();
    ASSERT_TRUE(receive(100, av_data_message(1, std::vector<std::byte>(8192, std::byte{1}))));
    ASSERT_TRUE(gate->wait_writes(1));
    ASSERT_TRUE(service_audio_completions(session, audio));
    ASSERT_EQ(transport.wire.sent_count(), before + 1);

    ASSERT_TRUE(receive(100, av_data_message(2, std::vector<std::byte>(19200, std::byte{2}))));
    ASSERT_TRUE(service_audio_completions(session, audio));
    // 8192 + 19200 bytes fit, but another full 100 ms envelope would exceed 200 ms.
    EXPECT_EQ(transport.wire.sent_count(), before + 1);
    EXPECT_FALSE(session.failure());
    gate->release();
    ASSERT_TRUE(wait_owner());
    EXPECT_EQ(transport.wire.sent_count(), before + 2);
    ASSERT_TRUE(service_audio_completions(session, audio));
    EXPECT_EQ(transport.wire.sent_count(), before + 2);
}

TEST_F(ReceiverAudioDispatch, SmallerVoiceWithholdsCreditUntilMaximumEnvelopeFits) {
    ASSERT_TRUE(start_audio_channel(101));
    const auto before = transport.wire.sent_count();
    ASSERT_TRUE(receive(101, av_data_message(1, std::vector<std::byte>(2048, std::byte{1}))));
    ASSERT_TRUE(gate->wait_writes(1));
    ASSERT_TRUE(service_audio_completions(session, audio));
    // A 6400-byte voice envelope alone consumes the role's entire 200 ms budget.
    EXPECT_EQ(transport.wire.sent_count(), before);
    gate->release();
    ASSERT_TRUE(wait_owner());
    ASSERT_EQ(transport.wire.sent_count(), before + 1);
    clock.set({base + 64'000'000});
    ASSERT_TRUE(receive(101, av_data_message(2, std::vector<std::byte>(6400, std::byte{2}))));
    ASSERT_TRUE(wait_owner());
    EXPECT_EQ(transport.wire.sent_count(), before + 2);
    ASSERT_TRUE(service_audio_completions(session, audio));
    EXPECT_EQ(transport.wire.sent_count(), before + 2);
    EXPECT_FALSE(session.failure());
}

TEST_F(ReceiverAudioDispatch, AllAdvertisedFirstEnvelopesFitBehindBacklogInDifferentOrders) {
    ASSERT_TRUE(start_audio_channel(101));
    ASSERT_TRUE(start_audio_channel(102));
    const std::array orders{
        std::array<std::uint8_t, 3>{100, 101, 102},
        std::array<std::uint8_t, 3>{101, 102, 100},
        std::array<std::uint8_t, 3>{102, 100, 101}};
    for (const auto &order : orders) {
        SCOPED_TRACE(static_cast<int>(order.front()));
        for (const auto channel : order) {
            ASSERT_TRUE(receive(channel, aa::protocol::encode(aa::protocol::AvSinkStop{}).value()));
            ASSERT_TRUE(receive(channel, aa::protocol::encode(aa::protocol::AvSinkStart{9, 0}).value()));
        }
        std::size_t first_write;
        {
            std::lock_guard lock(gate->mutex);
            gate->released = false;
            first_write = gate->writes + 1;
        }
        const auto before = transport.wire.sent_count();
        std::size_t parents = 0;
        // Fill only with granted credit; the other two roles have not sent their first message.
        for (; parents < 4;) {
            const auto sent = transport.wire.sent_count();
            const auto bytes = order.front() == 100 ? 8192U : 2048U;
            ASSERT_TRUE(receive(order.front(), av_data_message(parents + 1,
                std::vector<std::byte>(bytes, std::byte{1}))));
            ++parents;
            if (parents == 1) { ASSERT_TRUE(gate->wait_writes(first_write)); }
            ASSERT_TRUE(service_audio_completions(session, audio));
            if (transport.wire.sent_count() == sent) { break; }
        }
        ASSERT_LT(parents, 4U) << "credit must stop before a fifth parent can arrive";
        for (std::size_t i = 1; i < order.size(); ++i) {
            const auto bytes = order[i] == 100 ? 19200U : 6400U;
            ASSERT_TRUE(receive(order[i], av_data_message(1,
                std::vector<std::byte>(bytes, std::byte{2}))));
            ++parents;
        }
        EXPECT_FALSE(session.failure());
        gate->release();
        ASSERT_TRUE(wait_owner());
        EXPECT_EQ(transport.wire.sent_count(), before + parents);
        ASSERT_TRUE(service_audio_completions(session, audio));
        EXPECT_EQ(transport.wire.sent_count(), before + parents);
        clock.set({clock.now().count + 300'000'000});
    }
}

TEST_F(ReceiverAudioDispatch, RepeatedMaximumMediaRestartDiscardsUnsentRemainderAndStaleAcks) {
    const std::vector<std::byte> stale(19200, std::byte{0x11});
    const std::vector<std::byte> current(19200, std::byte{0x22});
    ASSERT_TRUE(receive(100, av_data_message(1, stale)));
    ASSERT_TRUE(gate->wait_writes(1));
    const auto before = transport.wire.sent_count();
    for (const int session_id : {-18, -19, -20}) {
        ASSERT_TRUE(receive(100, aa::protocol::encode(aa::protocol::AvSinkStop{}).value()));
        ASSERT_TRUE(receive(100, aa::protocol::encode(aa::protocol::AvSinkStart{session_id, 0}).value()));
        ASSERT_TRUE(receive(100, av_data_message(1, session_id == -20 ? current : stale)));
    }
    gate->release();
    ASSERT_TRUE(wait_owner());
    ASSERT_TRUE(service_audio_completions(session, audio));
    ASSERT_EQ(transport.wire.sent_count(), before + 1);
    const auto frame = aa::transport::decode_frame(transport.wire.last_sent());
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame.value().header.channel, 100);
    EXPECT_EQ(cipher->open(frame.value().payload).value(), aa::protocol::encode_av_sink_ack(-20).value());
    ASSERT_TRUE(audio.finish());
    std::vector<std::byte> rendered;
    for (const auto &chunk : gate->pcm) { rendered.insert(rendered.end(), chunk.begin(), chunk.end()); }
    ASSERT_GE(rendered.size(), current.size());
    // The current parent waits behind one detached 50 ms stale step, so the
    // unchanged live-PCM drift policy may head-trim it by at most 5 ms (960 B).
    constexpr std::size_t kMaxDriftTrimBytes = 960;
    std::size_t current_bytes = 0;
    while (current_bytes < rendered.size() && rendered[rendered.size() - 1 - current_bytes] == current.front()) {
        ++current_bytes;
    }
    EXPECT_LE(current_bytes, current.size());
    EXPECT_GE(current_bytes, current.size() - kMaxDriftTrimBytes);
    // At most one already-detached output step may survive; never the whole stale 100 ms parent.
    EXPECT_LT(rendered.size(), stale.size() + current.size());
    EXPECT_FALSE(audio.outstanding());
    EXPECT_FALSE(audio.take_completion());
}

TEST_F(ReceiverAudioDispatch, SupportedEnvelopeOversizeIsProtocolErrorRatherThanQueuePressure) {
    ASSERT_TRUE(start_audio_channel(101));
    ASSERT_TRUE(start_audio_channel(102));
    for (const std::uint8_t channel : {std::uint8_t{100}, std::uint8_t{101}, std::uint8_t{102}}) {
        SCOPED_TRACE(static_cast<int>(channel));
        const auto before = transport.wire.sent_count();
        const auto bytes = channel == 100 ? 19204U : 6402U;
        const auto rejected = receive(channel, av_data_message(1,
            std::vector<std::byte>(bytes, std::byte{1})));
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code(), aa::ErrorCode::protocol_payload_oversize);
        EXPECT_EQ(transport.wire.sent_count(), before);
    }
    EXPECT_FALSE(audio.outstanding());
    EXPECT_FALSE(audio.take_completion());
}

TEST_F(ReceiverAudioDispatch, VariableMediaParentsRenderFifoAndCreditExactlyOnce) {
    const std::array<std::size_t, 4> sizes{8192, 19200, 8192, 19200};
    std::vector<std::byte> expected;
    auto timestamp = base;
    const auto before = transport.wire.sent_count();
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        SCOPED_TRACE(i);
        const std::vector<std::byte> pcm(sizes[i], std::byte{static_cast<unsigned char>(i + 1)});
        expected.insert(expected.end(), pcm.begin(), pcm.end());
        clock.set({timestamp});
        ASSERT_TRUE(receive(100, av_data_message(i + 1, pcm)));
        if (i == 0) { ASSERT_TRUE(gate->wait_writes(1)); }
        ASSERT_TRUE(service_audio_completions(session, audio));
        if (i == 0) { ASSERT_EQ(transport.wire.sent_count(), before + 1); }
        if (i == 1) { gate->release(); }
        if (i >= 1) {
            ASSERT_TRUE(wait_owner());
            EXPECT_EQ(transport.wire.sent_count(), before + i + 1);
            ASSERT_TRUE(service_audio_completions(session, audio));
            EXPECT_EQ(transport.wire.sent_count(), before + i + 1);
        }
        timestamp += static_cast<std::int64_t>(sizes[i]) * 1'000'000'000LL / 192000;
    }
    ASSERT_TRUE(audio.finish());
    std::vector<std::byte> actual;
    for (const auto &chunk : gate->pcm) { actual.insert(actual.end(), chunk.begin(), chunk.end()); }
    EXPECT_EQ(actual, expected);
    EXPECT_FALSE(audio.take_completion());
}

TEST_F(ReceiverAudioDispatch, VariableVoiceParentCreditsRemainFifoAndExactlyOnce) {
    using aa::protocol::ChannelRole;
    const std::array roles{ChannelRole::guidance_audio, ChannelRole::system_audio};
    const std::array<std::size_t, 4> sizes{2048, 6400, 2048, 6400};
    gate->release();
    auto timestamp = base;
    for (const auto role : roles) {
        SCOPED_TRACE(static_cast<int>(role));
        audio.invalidate(role, 100);
        std::vector<aa::channels::AvDeliveryId> admitted;
        std::vector<aa::channels::AvDeliveryId> credited;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            const aa::channels::AvDeliveryId id{role, 100, i + 1, 7};
            const std::vector<std::byte> pcm(sizes[i], std::byte{1});
            ASSERT_TRUE(audio.submit({{role, i + 1, {timestamp}}, pcm}, id));
            admitted.push_back(id);
            const auto credit = audio.take_completion(std::chrono::steady_clock::now() +
                                                       std::chrono::seconds{2});
            ASSERT_TRUE(credit);
            credited.push_back(*credit);
            timestamp += static_cast<std::int64_t>(sizes[i]) * 1'000'000'000LL / 32000;
        }
        ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        EXPECT_EQ(credited, admitted);
        EXPECT_FALSE(audio.take_completion());
    }
}

TEST_F(ReceiverAudioDispatch, RepeatedInvalidationReclaimsMaximumParentsWithoutDuplicateCredit) {
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(19200, std::byte{1});
    audio.invalidate(role, 100);
    ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 100, 1, 7}));
    ASSERT_TRUE(gate->wait_writes(1));
    for (std::uint64_t generation = 101; generation <= 104; ++generation) {
        audio.invalidate(role, generation);
        audio.invalidate(role, generation);
        EXPECT_FALSE(audio.take_completion());
        ASSERT_TRUE(audio.submit({{role, generation, {base}}, pcm}, {role, generation, 1, 7}));
    }
    std::vector<aa::channels::AvDeliveryId> credited;
    while (const auto credit = audio.take_completion()) { credited.push_back(*credit); }
    gate->release();
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    while (const auto credit = audio.take_completion()) { credited.push_back(*credit); }
    EXPECT_EQ(credited, (std::vector<aa::channels::AvDeliveryId>{{role, 104, 1, 7}}));
    const aa::channels::AvDeliveryId next{role, 104, 2, 7};
    ASSERT_TRUE(audio.submit({{role, 2, {base + 100'000'000}}, pcm}, next));
    const auto credit = audio.take_completion(std::chrono::steady_clock::now() + std::chrono::seconds{2});
    ASSERT_TRUE(credit);
    EXPECT_EQ(*credit, next);
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    EXPECT_FALSE(audio.take_completion());
    EXPECT_FALSE(audio.failure());
    EXPECT_FALSE(audio.outstanding());
}

TEST_F(ReceiverAudioDispatch, PollServiceBudgetIsNonterminalButSessionCancellationControls) {
    const auto idle = session.poll_transport(std::chrono::steady_clock::now());
    ASSERT_TRUE(idle);
    EXPECT_FALSE(idle.value());
    EXPECT_FALSE(session.failure());
    EXPECT_FALSE(session.token().stop_requested());
    EXPECT_EQ(transport.actual_deadline, aa::core::Deadline::Clock::time_point::max());
    const auto polls = transport.polls;
    session.request_stop();
    const auto cancelled = session.poll_transport(std::chrono::steady_clock::now() +
                                                  std::chrono::milliseconds{5});
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code(), aa::ErrorCode::cancelled);
    EXPECT_EQ(transport.polls, polls);
    EXPECT_EQ(session.state(), aa::session::State::stopping);
}

TEST_F(ReceiverAudioDispatch, PollJsonDistinguishesAvPendingDataAndConfigAndSurvivesFailure) {
    ASSERT_TRUE(receive(100, av_data_message(1, wiredtest::pcm_unit())));
    ASSERT_TRUE(gate->wait_writes(1));
    const auto before = transport.wire.sent_count();
    for (const bool config : {false, true}) {
        const auto result = receive(100, config
            ? std::vector<std::byte>{std::byte{0}, std::byte{1}}
            : av_data_message(2, wiredtest::pcm_unit()));
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), aa::ErrorCode::ipc_queue_full);
        const auto json = poll_error_json(result.error().code(), session, audio);
        EXPECT_NE(json.find(config ? "\"reason\":\"codec_config\"" : "\"reason\":\"data\""),
                  std::string::npos) << json;
        EXPECT_NE(json.find("\"source\":\"av_pending\""), std::string::npos) << json;
        EXPECT_NE(json.find("\"items\":1,\"capacity\":1"), std::string::npos) << json;
        EXPECT_EQ(json.find("reject_channel"), std::string::npos);
        EXPECT_EQ(transport.wire.sent_count(), before);
    }
    const auto saved = session.last_queue_rejection();
    ASSERT_TRUE(session.fail(aa::Error{aa::ErrorCode::ipc_queue_full}));
    EXPECT_EQ(session.last_queue_rejection(), saved);
    ASSERT_TRUE(service_audio_completions(session, audio));
    EXPECT_EQ(session.last_queue_rejection(), saved);
    session.attach_transport(&transport);
    EXPECT_FALSE(session.last_queue_rejection());
}

TEST_F(ReceiverAudioDispatch, PollJsonReportsWorkerSlotCapacityAndClearsOnNextPoll) {
    const std::vector<std::byte> pcm(4, std::byte{1});
    const auto role = aa::protocol::ChannelRole::media_audio;
    audio.invalidate(role, 100);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        ASSERT_TRUE(audio.submit({{role, i, {base}}, pcm}, {role, 100, i, -17}));
        if (i == 1) { ASSERT_TRUE(gate->wait_writes(1)); }
    }
    const auto before = transport.wire.sent_count();
    // Deliberately bypass the AV credit gate: the fifth parent is unauthorized.
    const auto result = audio.submit({{role, 5, {base}}, pcm}, {role, 100, 5, -17});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::ipc_queue_full);
    const auto json = poll_error_json(result.error().code(), {}, audio.last_queue_rejection(),
                                      audio.current_rejection_snapshot());
    EXPECT_NE(json.find("\"source\":\"audio_worker\",\"reason\":\"slots\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"items\":4,\"capacity\":12,\"bytes\":16,\"byte_capacity\":51200,\"units\":16"),
              std::string::npos) << json;
    EXPECT_NE(json.find("\"requested_bytes\":4,\"requested_units\":4"), std::string::npos) << json;
    const auto frozen = audio.rejection_snapshot();
    ASSERT_TRUE(frozen);
    ASSERT_TRUE(frozen->active);
    EXPECT_FALSE(frozen->active->write);
    EXPECT_EQ(frozen->active->original_scheduled.count, base);
    EXPECT_EQ(frozen->active->delivery.session_id, -17);
    EXPECT_EQ(frozen->active->delivery.sequence, 1U);
    EXPECT_GT(frozen->active->delivery.generation, 0U);
    ASSERT_TRUE(frozen->active->dequeued_at);
    EXPECT_LE(frozen->active->received_at, *frozen->active->dequeued_at);
    EXPECT_LE(*frozen->active->dequeued_at, frozen->rejected_at);
    ASSERT_TRUE(frozen->oldest_queued_receipt);
    EXPECT_GE(*frozen->oldest_queued_receipt, frozen->active->received_at);
    EXPECT_LE(*frozen->oldest_queued_receipt, frozen->rejected.received_at);
    EXPECT_EQ(frozen->occupied_by_role, (std::array<std::size_t, 4>{4, 0, 0, 0}));
    EXPECT_EQ(frozen->pending_completions_by_role, (std::array<std::size_t, 4>{3, 0, 0, 0}));
    EXPECT_NE(json.find("\"audio_rejection\":"), std::string::npos);
    EXPECT_NE(json.find("\"write\":null"), std::string::npos);
    EXPECT_EQ(json.find("\"pcm\""), std::string::npos);
    EXPECT_EQ(json.find("\"payload\""), std::string::npos);
    while (audio.take_completion()) {}
    EXPECT_EQ(transport.wire.sent_count(), before);
    ASSERT_TRUE(receive(0, aa::protocol::encode(aa::protocol::PingRequest{42, {}}).value()));
    EXPECT_FALSE(session.last_queue_rejection());
    EXPECT_EQ(poll_error_json(aa::ErrorCode::ipc_queue_full, session, audio),
              "{\"event\":\"err\",\"detail\":\"poll\",\"code\":\"ipc_queue_full\"}");
    ASSERT_TRUE(session.fail(result.error()));
    EXPECT_EQ(audio.rejection_snapshot(), frozen);
    gate->release();
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(audio.finish());
    EXPECT_EQ(audio.rejection_snapshot(), frozen);
    EXPECT_EQ(transport.wire.sent_count(), before + 1); // Only the explicit ping replied.
}

TEST_F(ReceiverAudioDispatch, RetainedForensicSnapshotDoesNotAttachAfterAcceptedWorkerSubmit) {
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(4, std::byte{1});
    audio.invalidate(role, 100);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        ASSERT_TRUE(audio.submit({{role, i, {base}}, pcm}, {role, 100, i, -17}));
        if (i == 1) { ASSERT_TRUE(gate->wait_writes(1)); }
    }
    ASSERT_FALSE(audio.submit({{role, 5, {base}}, pcm}, {role, 100, 5, -17}));
    const auto frozen = audio.rejection_snapshot();
    ASSERT_TRUE(frozen);
    ASSERT_TRUE(frozen->active);
    const auto sent = transport.wire.sent_count();
    gate->release();
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    EXPECT_EQ(audio.rejection_snapshot(), frozen);
    while (audio.take_completion()) {} // Credits withheld during the hold, granted after render.
    const aa::channels::AvDeliveryId fresh{role, frozen->active->delivery.generation, 99, -17};
    const auto fresh_submitted = audio.submit({{role, 99, {base + 100'000}}, pcm}, fresh);
    ASSERT_TRUE(fresh_submitted) << aa::to_string(fresh_submitted.error().code());
    EXPECT_EQ(audio.take_completion(), fresh);
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    EXPECT_FALSE(audio.last_queue_rejection());
    EXPECT_EQ(audio.rejection_snapshot(), frozen);
    const auto unrelated = poll_error_json(aa::ErrorCode::ipc_queue_full, {}, frozen->queue,
                                          audio.current_rejection_snapshot());
    EXPECT_NE(unrelated.find("\"queue_rejection\":"), std::string::npos);
    EXPECT_EQ(unrelated.find("\"audio_rejection\":"), std::string::npos) << unrelated;
    audio.invalidate(role, fresh.generation + 1);
    EXPECT_EQ(audio.rejection_snapshot(), frozen);
    auto other_queue = frozen->queue;
    other_queue.source = aa::diagnostics::QueueSource::av_pending;
    const auto other = poll_error_json(aa::ErrorCode::ipc_queue_full, {}, other_queue, frozen);
    EXPECT_EQ(other.find("\"audio_rejection\":"), std::string::npos);
    ASSERT_TRUE(audio.finish());
    EXPECT_EQ(audio.rejection_snapshot(), frozen);
    EXPECT_EQ(transport.wire.sent_count(), sent);
}

TEST_F(ReceiverAudioDispatch, TransientStallWithholdsCreditInsteadOfOverflowAndRestoresExactCapacity) {
    const std::vector<std::byte> pcm(8192, std::byte{0});
    const auto deliver = [&](std::uint64_t index) {
        clock.set({base + static_cast<std::int64_t>(index) * 8192 * 1'000'000'000LL / 192000});
        auto frames = aa::transport::encode_message(
            {100, aa::transport::MessageKind::specific, av_data_message(index, pcm)},
            aa::transport::Encryption::encrypted, cipher);
        EXPECT_TRUE(frames);
        if (!frames) { return aa::core::Result<bool>{frames.error()}; }
        EXPECT_EQ(frames.value().size(), 1U);
        transport.push(std::move(frames.value().front()));
        return receive_with_audio(session, audio);
    };
    const auto expect_last_ack = [&] {
        const auto frame = aa::transport::decode_frame(transport.wire.last_sent());
        ASSERT_TRUE(frame);
        EXPECT_EQ(frame.value().header.channel, 100);
        EXPECT_EQ(cipher->open(frame.value().payload).value(), aa::protocol::encode_av_sink_ack(-17).value());
    };
    // Stalled first write: chunks 1-2 are credited at once, chunk 3 is
    // admitted but its credit is withheld, so a conforming peer stops.
    const auto before = transport.wire.sent_count();
    for (std::uint64_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(deliver(i));
        if (i == 0) { ASSERT_TRUE(gate->wait_writes(1)); }
        EXPECT_EQ(transport.wire.sent_count(), before + std::min<std::size_t>(i + 1, 2));
    }
    ASSERT_TRUE(receive(99, av_data_message(1, wiredtest::kVideoIdr)));
    ASSERT_TRUE(consumer->receive(Milliseconds{500}));
    ASSERT_TRUE(receive(0, aa::protocol::encode(aa::protocol::PingRequest{42, {}}).value()));
    EXPECT_EQ(session.pings_answered(), 1U);
    ASSERT_TRUE(service_audio_completions(session, audio));
    const auto held = transport.wire.sent_count();
    EXPECT_TRUE(audio.outstanding());
    EXPECT_FALSE(session.failure());
    gate->release();
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(service_audio_completions(session, audio));
    EXPECT_EQ(transport.wire.sent_count(), held + 1); // The withheld credit, exactly once.
    expect_last_ack();
    ASSERT_TRUE(service_audio_completions(session, audio));
    EXPECT_EQ(transport.wire.sent_count(), held + 1);
    EXPECT_FALSE(audio.outstanding());
    {
        std::lock_guard lock(gate->mutex);
        gate->released = false;
    }
    // Same stall again from an idle queue: identical credit pattern.
    const auto refilled = transport.wire.sent_count();
    for (std::uint64_t i = 3; i < 6; ++i) {
        ASSERT_TRUE(deliver(i));
        if (i == 3) { ASSERT_TRUE(gate->wait_writes(4)); }
        EXPECT_EQ(transport.wire.sent_count(), refilled + std::min<std::size_t>(i - 2, 2));
    }
    // A peer ignoring the withheld credit is rejected by the one-pending AV
    // contract before reaching the worker, which never exceeds three jobs.
    const auto violated = deliver(6);
    ASSERT_FALSE(violated);
    EXPECT_EQ(violated.error().code(), aa::ErrorCode::ipc_queue_full);
    const auto snapshot = session.last_queue_rejection();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->source, aa::diagnostics::QueueSource::av_pending);
    EXPECT_EQ(snapshot->reason, aa::diagnostics::QueueReason::data);
    EXPECT_FALSE(audio.last_queue_rejection());
    EXPECT_EQ(transport.wire.sent_count(), refilled + 2);
    ASSERT_TRUE(session.fail(violated.error()));
    gate->release();
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    EXPECT_FALSE(audio.outstanding());
    EXPECT_FALSE(audio.failure());
    EXPECT_EQ(gate->writes, 4U); // Three completed parents (one 50 ms step each) and one detached step.
    std::cout << "STALL_ACCOUNTING logical_period_ns=42666666 credited_before_hold=2"
                 " withheld_credit_granted_after_release=1 max_jobs=3 overflow=0"
                 " video=1 ping=1 idle_after_terminal_release=1\n";
}

// Decorates, but never substitutes for, the real backend's write/barrier result.
// Observations are read only after actual-main has joined/destroyed its worker.
struct CadencedGstObservation {
    bool opened = false;
    bool armed = false;
    bool entered = false;
    bool drained = false;
    std::int64_t held_at = 0;
    std::int64_t released_at = 0;
    aa::audio::detail::AppsrcQueueLevel held_queue;
    aa::audio::RenderBarrierStats held_render;
    aa::audio::RenderBarrierStats after_render;
    std::vector<std::int64_t> write_at;
    std::vector<std::int64_t> scheduled_at;
    std::vector<std::int64_t> returned_at;
    std::vector<bool> succeeded;
};

class CadencedRealGstSink final : public aa::audio::detail::ObservedAudioSink {
  public:
    CadencedRealGstSink(std::unique_ptr<aa::audio::detail::ObservedAudioSink> sink,
                       CadencedGstObservation &observation, int hold_ms, // NOLINT(bugprone-easily-swappable-parameters): milliseconds, not the following write count
                       std::size_t hold_after_writes = 0)
        : sink_(std::move(sink)), observation_(observation), hold_ms_(hold_ms),
          hold_after_writes_(hold_after_writes) {
        if (hold_ms_ != 0 && hold_after_writes_ == 0) { arm(); }
    }
    void arm() {
        if (armed_ || hold_ms_ == 0) { return; }
        armed_ = true;
        observation_.armed = aa::audio::detail::block_downstream_for_test(*sink_);
        if (!observation_.armed) { return; }
        release_ = std::thread([this, hold_ms = hold_ms_] {
            observation_.entered = aa::audio::detail::wait_downstream_block_entered_for_test(
                *sink_, Milliseconds{2000});
            if (observation_.entered) {
                observation_.held_at = steady_ns();
                observation_.held_queue = aa::audio::detail::appsrc_queue_level_for_test(*sink_);
                observation_.held_render = sink_->render_stats();
                std::mutex mutex;
                std::condition_variable cv;
                std::unique_lock lock(mutex);
                cv.wait_until(lock, std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds{hold_ms}, [] { return false; });
            }
            observation_.released_at = steady_ns();
            aa::audio::detail::unblock_downstream_for_test(*sink_);
        });
    }
    ~CadencedRealGstSink() override {
        if (release_.joinable()) { release_.join(); }
        aa::audio::detail::unblock_downstream_for_test(*sink_);
        observation_.after_render = sink_->render_stats();
    }
    aa::core::Result<void> write(const aa::audio::MixedChunk &chunk) override {
        if (hold_after_writes_ != 0 && observation_.write_at.size() + 1 == hold_after_writes_) { arm(); }
        observation_.write_at.push_back(steady_ns());
        observation_.scheduled_at.push_back(chunk.timestamp.count);
        auto result = sink_->write(chunk);
        observation_.returned_at.push_back(steady_ns());
        observation_.succeeded.push_back(result.has_value());
        return result;
    }
    aa::core::Result<void> drain(Milliseconds timeout) override {
        auto result = sink_->drain(timeout);
        observation_.drained = result.has_value();
        return result;
    }
    aa::audio::SinkProbeStats probe_stats() const override { return sink_->probe_stats(); }
    aa::audio::RenderBarrierStats render_stats() const override { return sink_->render_stats(); }
  private:
    std::unique_ptr<aa::audio::detail::ObservedAudioSink> sink_;
    CadencedGstObservation &observation_;
    std::thread release_;
    int hold_ms_ = 0;
    std::size_t hold_after_writes_ = 0;
    bool armed_ = false;
};

TEST(WiredReceiver, ActualMainRealGstIndependentCadenceReversibleHoldControls) {
    for (const int hold_ms : {0, 50, 300}) {
        SCOPED_TRACE(hold_ms);
        CadencedPcmSource source;
        CadencedGstObservation observed;
        runtime_send_audio = true;
        runtime_pcm_source = &source;
        runtime_audio_ack_attempts.store(0);
        runtime_sink_request_override = [&](const aa::audio::detail::SinkOpenRequest &request) {
            auto outcome = real_runtime_sink(request);
            observed.opened = outcome.sink != nullptr;
            if (outcome.sink) {
                outcome.sink = std::make_unique<CadencedRealGstSink>(
                    std::move(outcome.sink), observed, hold_ms);
            }
            return outcome;
        };
        const auto result = run_runtime(false, 1200, true);
        runtime_sink_request_override = {};
        runtime_pcm_source = nullptr;
        runtime_send_audio = false;
        if (!observed.opened) {
            GTEST_SKIP() << "real host GStreamer route unavailable; hold_ms=" << hold_ms
                         << " actual-main output=" << result.output;
        }
        const auto epoch_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            source.epoch.time_since_epoch()).count();
        std::cout << "MAIN_GST hold_ms=" << hold_ms << " exit=" << result.code
                  << " arrivals=" << source.arrivals.size() << " acks=" << source.acks.size()
                  << " armed=" << observed.armed << " entered=" << observed.entered
                  << " held_at_ns=" << (observed.entered ? observed.held_at - epoch_ns : 0)
                  << " released_at_ns=" << (observed.entered ? observed.released_at - epoch_ns : 0)
                  << " held_bytes=" << observed.held_queue.queued_bytes
                  << " held_buffers=" << observed.held_queue.queued_buffers
                  << " held_marker=" << observed.held_render.chain_return_buffers
                  << " held_handoff=" << observed.held_render.handoff_events
                  << " final_marker=" << observed.after_render.chain_return_buffers
                  << " final_handoff=" << observed.after_render.handoff_events << '\n';
        for (std::size_t i = 0; i < source.arrivals.size(); ++i) {
            const auto due_ns = static_cast<std::int64_t>(i) * 8192 * 1'000'000'000LL / 192000;
            std::cout << "MAIN_GST_ARRIVAL chunk=" << i + 1 << " due_ns=" << due_ns
                      << " arrival_ns=" << source.arrivals[i];
            if (i < source.acks.size()) { std::cout << " ack_ns=" << source.acks[i]; }
            std::cout << '\n';
            EXPECT_GE(source.arrivals[i], due_ns);
            if (i < source.acks.size()) {
                EXPECT_GE(source.acks[i], source.arrivals[i]);
                if (i + 1 < source.arrivals.size()) { EXPECT_LE(source.acks[i], source.arrivals[i + 1]); }
            }
        }
        for (std::size_t i = 0; i < observed.returned_at.size(); ++i) {
            std::cout << "MAIN_GST_WRITE write=" << i + 1
                      << " scheduled_ns=" << observed.scheduled_at[i] - epoch_ns
                      << " entered_ns=" << observed.write_at[i] - epoch_ns
                      << " returned_ns=" << observed.returned_at[i] - epoch_ns
                      << " success=" << observed.succeeded[i] << '\n';
        }
        std::cout << result.output;
        EXPECT_EQ(runtime_audio_ack_attempts.load(), static_cast<int>(source.acks.size()));
        EXPECT_EQ(runtime_connects.load(), 1);
        EXPECT_EQ(result.output.find("\"to_state\":\"reconnecting\""), std::string::npos);
        ASSERT_FALSE(observed.succeeded.empty());
        EXPECT_TRUE(std::all_of(observed.succeeded.begin(), observed.succeeded.end(),
                                [](bool succeeded) { return succeeded; }));
        EXPECT_TRUE(observed.drained);
        if (hold_ms != 0) {
            ASSERT_TRUE(observed.armed);
            ASSERT_TRUE(observed.entered);
            EXPECT_EQ(observed.held_render.chain_return_buffers, 0U);
            EXPECT_EQ(observed.held_render.handoff_events, 0U);
            EXPECT_GE(observed.returned_at.front(), observed.released_at);
            EXPECT_GT(observed.after_render.handoff_events, observed.held_render.handoff_events);
        }
        EXPECT_EQ(result.output.find("ipc_queue_full"), std::string::npos);
        ASSERT_EQ(result.code, 0) << result.output;
        ASSERT_EQ(source.arrivals.size(), 8U);
        ASSERT_EQ(source.acks.size(), 8U);
        EXPECT_EQ(result.output.find("ipc_queue_full"), std::string::npos);
        EXPECT_EQ(result.output.find("\"event\":\"err\""), std::string::npos);
        EXPECT_EQ(result.output.find("\"audio_rejection\":"), std::string::npos);
        if (hold_ms == 300) {
            // The held first write keeps the third chunk's credit withheld
            // until the real backend returns; nothing is over-admitted.
            EXPECT_NE(result.output.find("\"event\":\"audio_credit_deferred\""), std::string::npos)
                << result.output;
            EXPECT_GE(source.acks[2], observed.returned_at.front() - epoch_ns);
            EXPECT_GT(source.arrivals[3], observed.released_at - epoch_ns);
        }
    }
}

TEST(WiredReceiver, RealGstReversibleBlockSeparatesPairAdmissionFromMarkerHandoff) {
    ReceiverLogScope logging{g_running};
    namespace detail = aa::audio::detail;
    for (const int scenario : {0, 1, 2, 3}) {
        const bool prefill = scenario % 2 != 0;
        const int hold_ms = scenario < 2 ? 50 : 300;
        detail::ObservedAudioSink *sink = nullptr;
        ConcreteAudioRenderWorker audio{pcm_router_config(), [&](const auto &config) {
            detail::SinkOpenRequest request;
            request.format = config.output_format;
            auto outcome = real_runtime_sink(request);
            sink = outcome.sink.get();
            return detail::open_router_with_sink_for_test(config, std::move(outcome.sink));
        }};
        const auto ready = audio.ready();
        if (!ready || !sink) { GTEST_SKIP() << "real host audio route unavailable"; }
        struct Unblock {
            detail::ObservedAudioSink &sink;
            ~Unblock() { detail::unblock_downstream_for_test(sink); }
        } unblock{*sink};
        ASSERT_TRUE(detail::block_downstream_for_test(*sink));
        if (prefill) {
            (void)detail::prefill_appsrc_for_test(*sink, 4);
            ASSERT_TRUE(detail::wait_downstream_block_entered_for_test(*sink, Milliseconds{2000}));
            ASSERT_GE(detail::prefill_appsrc_for_test(*sink, 65536), 65536U);
        }
        const auto role = aa::protocol::ChannelRole::media_audio;
        const std::vector<std::byte> pcm(8192, std::byte{0});
        const auto started = steady_ns();
        ASSERT_TRUE(audio.submit({{role, 1, {started}}, pcm}, {role, 0, 1, 1}));
        ASSERT_TRUE(audio.take_completion());
        ASSERT_TRUE(detail::wait_downstream_block_entered_for_test(*sink, Milliseconds{2000}));
        EXPECT_FALSE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::milliseconds{hold_ms}));
        const auto level = detail::appsrc_queue_level_for_test(*sink);
        const auto probe = sink->probe_stats();
        const auto barrier = sink->render_stats();
        EXPECT_EQ(barrier.handoff_events, 0U);
        EXPECT_EQ(barrier.chain_return_buffers, 0U);
        EXPECT_EQ(probe.buffers, 0U);
        if (prefill) { EXPECT_GE(level.queued_bytes, 65536U); }
        else { EXPECT_LT(level.queued_bytes, 65536U); }
        const auto released = steady_ns();
        detail::unblock_downstream_for_test(*sink);
        ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        EXPECT_FALSE(audio.failure());
        const auto after = sink->render_stats();
        EXPECT_GE(after.handoff_events, 1U);
        EXPECT_GE(after.chain_return_buffers, 1U);
        std::cout << "STALL_GST phase=" << (prefill ? "pair_admission" : "post_pair_pre_marker")
                  << " hold_ms=" << hold_ms << " held_ns=" << released - started << " queued_bytes=" << level.queued_bytes
                  << " queued_buffers=" << level.queued_buffers << " ingress=" << probe.buffers
                  << " marker=" << barrier.chain_return_buffers << " handoff=" << barrier.handoff_events
                  << " recovered_marker=" << after.chain_return_buffers
                  << " recovered_handoff=" << after.handoff_events << '\n';
    }
}

TEST_F(ReceiverAudioDispatch, PollJsonReportsWorkerByteCapacityBeforeWeightedCapacity) {
    const std::vector<std::byte> pcm(19200, std::byte{1});
    const auto role = aa::protocol::ChannelRole::media_audio;
    audio.invalidate(role, 100);
    ASSERT_TRUE(start_audio_channel(101));
    for (std::uint64_t i = 0; i < 2; ++i) {
        ASSERT_TRUE(audio.submit({{role, i, {base}}, pcm}, {role, 100, i, 7}));
        if (i == 0) { ASSERT_TRUE(gate->wait_writes(1)); }
    }
    ASSERT_TRUE(receive(101, av_data_message(2, std::vector<std::byte>(1600, std::byte{1}))));
    const auto result = audio.submit({{role, 99, {base}}, std::span{pcm}.first(4)}, {role, 100, 99, 7});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::ipc_queue_full);
    const auto json = poll_error_json(result.error().code(), {}, audio.last_queue_rejection(),
                                      audio.current_rejection_snapshot());
    EXPECT_NE(json.find("\"source\":\"audio_worker\",\"reason\":\"bytes\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"items\":3,\"capacity\":12,\"bytes\":40000,\"byte_capacity\":51200,\"units\":48000"),
              std::string::npos) << json;
    const auto frozen = audio.rejection_snapshot();
    ASSERT_TRUE(frozen);
    ASSERT_TRUE(session.fail(result.error()));
    EXPECT_EQ(poll_error_json(result.error().code(), {}, frozen->queue, frozen), json);
    EXPECT_EQ(audio.rejection_snapshot(), frozen);
    gate->release();
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(audio.finish());
    EXPECT_EQ(audio.rejection_snapshot(), frozen);
    EXPECT_EQ(poll_error_json(result.error().code(), {}, frozen->queue, frozen), json);
}

TEST_F(ReceiverAudioDispatch, PollJsonReportsWorkerWeightedUnitsAcrossAudioRoles) {
    ASSERT_TRUE(receive(100, av_data_message(1, std::vector<std::byte>(19200, std::byte{1}))));
    ASSERT_TRUE(gate->wait_writes(1));
    ASSERT_TRUE(service_audio_completions(session, audio));
    for (const std::uint8_t channel : {std::uint8_t{101}, std::uint8_t{102}}) {
        ASSERT_TRUE(receive(channel, aa::protocol::encode(aa::protocol::AvSinkSetup{}).value()));
        ASSERT_TRUE(receive(channel, aa::protocol::encode(aa::protocol::AvSinkStart{7, 0}).value()));
        ASSERT_TRUE(receive(channel, av_data_message(1, std::vector<std::byte>(1600, std::byte{1}))));
        ASSERT_TRUE(service_audio_completions(session, audio));
    }
    ASSERT_TRUE(receive(100, av_data_message(2, std::vector<std::byte>(4, std::byte{1}))));
    const auto role = aa::protocol::ChannelRole::guidance_audio;
    audio.invalidate(role, 100);
    const std::vector<std::byte> malformed(6399, std::byte{1});
    ASSERT_TRUE(audio.submit({{role, 98, {base}}, malformed}, {role, 100, 98, 7}));
    // Unauthorized short frames still round UP in the occupancy ledger.
    const auto result = audio.submit({{role, 99, {base}}, std::span{malformed}.first(1)}, {role, 100, 99, 7});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::ipc_queue_full);
    const auto json = poll_error_json(result.error().code(), {}, audio.last_queue_rejection(),
                                      audio.current_rejection_snapshot());
    EXPECT_NE(json.find("\"source\":\"audio_worker\",\"reason\":\"weighted_units\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"items\":4,\"capacity\":12,\"bytes\":27203,\"byte_capacity\":51200,\"units\":67204"),
              std::string::npos) << json;
    EXPECT_NE(json.find("\"requested_bytes\":1,\"requested_units\":12"), std::string::npos) << json;
    ASSERT_TRUE(session.fail(result.error()));
    EXPECT_EQ(poll_error_json(result.error().code(), {}, audio.last_queue_rejection(),
                              audio.current_rejection_snapshot()), json);
}

TEST(WiredReceiver, WorkerSettledIdleResumesOriginalTimelineAndReportsPrefixExactlyOnce) {
    ReceiverLogScope logging{g_running};
    StdoutCaptureGuard capture;
    auto gate = std::make_shared<RenderGate>();
    gate->release();
    const auto base = steady_ns() - 10'000'000'000LL;
    std::atomic<std::int64_t> now{base + 5'000'000};
    ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate),
        [&] { return aa::core::Nanoseconds{now.load()}; }};
    ASSERT_TRUE(audio.ready());
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(960, std::byte{1}); // 240 frames / 5 ms
    ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 0, 1, 7}));
    ASSERT_TRUE(audio.take_completion(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    now.store(base + 1'205'000'000);
    // Repeated empty reconciliation must not restart the settled interval.
    audio.invalidate(aa::protocol::ChannelRole::guidance_audio, 1);
    audio.invalidate(aa::protocol::ChannelRole::guidance_audio, 2);
    now.store(base + 2'405'000'000);
    ASSERT_TRUE(audio.submit({{role, 2, {base + 2'405'000'000}}, pcm}, {role, 0, 2, 7}));
    ASSERT_TRUE(audio.take_completion(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_FALSE(audio.failure());
    // A normal next parent neither extends the observation nor logs another resume.
    ASSERT_TRUE(audio.submit({{role, 3, {base + 2'410'000'000}}, pcm}, {role, 0, 3, 7}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(audio.finish());
    const auto output = capture.release();
    const auto report = audio.final_report();
    ASSERT_TRUE(report);
    EXPECT_EQ(report->stats.frames_idle_skipped, 115200U);
    EXPECT_EQ(report->stats.frames_skipped, 0U);
    EXPECT_EQ(report->stats.frames_silence, 0U);
    EXPECT_EQ(report->stats.frames_written, 3U);
    EXPECT_EQ(report->stats.frames_output, 720U);
    EXPECT_EQ(gate->timestamps, (std::vector<aa::core::Nanoseconds>{
        {base}, {base + 2'405'000'000}, {base + 2'410'000'000}}));
    const auto resume = "{\"event\":\"audio_idle_resume\",\"id\":1,\"start_ns\":" +
        std::to_string(base + 5'000'000) + ",\"end_ns\":" + std::to_string(base + 2'405'000'000) +
        ",\"idle_ns\":2400000000,\"reason\":\"all_role_idle\"}";
    EXPECT_NE(output.find(resume), std::string::npos) << output;
    EXPECT_NE(output.find("{\"event\":\"audio_idle_skipped\",\"id\":1,\"frames_idle_skipped\":115200,"
                          "\"frames_idle_skipped_total\":115200}"), std::string::npos) << output;
    const auto first_resume = output.find("\"event\":\"audio_idle_resume\"");
    const auto first_skip = output.find("\"event\":\"audio_idle_skipped\"");
    ASSERT_NE(first_resume, std::string::npos);
    ASSERT_NE(first_skip, std::string::npos);
    EXPECT_EQ(output.find("\"event\":\"audio_idle_resume\"", first_resume + 1), std::string::npos);
    EXPECT_EQ(output.find("\"event\":\"audio_idle_skipped\"", first_skip + 1), std::string::npos);
}

struct CompletionBarrier {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<aa::audio::WriteCompletion> observed;
    bool blocked = false;
    bool released = false;
    bool timed_out = false;
    void release() {
        const std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
    void complete(const aa::audio::WriteCompletion &value) {
        std::unique_lock lock(mutex);
        observed.push_back(value);
        if (observed.size() != 2) { return; }
        blocked = true;
        cv.notify_all();
        if (!cv.wait_for(lock, std::chrono::seconds{2}, [&] { return released; })) {
            timed_out = true;
            released = true;
            cv.notify_all();
        }
    }
    bool wait_blocked() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds{2}, [&] { return blocked; });
    }
};

enum class CompletionIdleOrder { receipt_before_settle, settle_before_advance, settle_after_advance };

void expect_post_completion_idle_order(CompletionIdleOrder order) {
    ReceiverLogScope logging{g_running};
    StdoutCaptureGuard capture;
    auto gate = std::make_shared<RenderGate>();
    gate->release();
    CompletionBarrier barrier;
    // All scheduled and injected times are historical, including the resumed
    // parent, so real-clock elapsed clipping cannot truncate its scheduled gap.
    const auto base = steady_ns() - 30'000'000'000LL;
    constexpr std::int64_t gap_ns = 18'395'020'834; // Floor alignment: exactly 882961 frames at 48 kHz.
    const auto second_end = base + 10'000'000;
    const auto third_start = second_end + gap_ns;
    std::atomic<std::int64_t> now{base + 5'000'000};
    auto config = pcm_router_config();
    config.on_write_completed = [&, normal = config.on_write_completed](const auto &value) {
        normal(value); // Forward the real completion observation BEFORE blocking.
        barrier.complete(value);
    };
    struct IdleObservation {
        std::array<std::size_t, 4> pending;
        bool in_flight;
        bool armed;
        aa::audio::RouterReport report;
    };
    std::vector<IdleObservation> settled;
    ConcreteAudioRenderWorker audio{config, gated_router(gate),
        [&] { return aa::core::Nanoseconds{now.load()}; }};
    // Destroyed before audio: fatal assertions also release the callback before
    // the worker destructor joins. The callback wait has its own bounded escape.
    struct ReleaseOnExit {
        CompletionBarrier &barrier;
        ~ReleaseOnExit() { barrier.release(); }
    } release_on_exit{barrier};
    const auto context = [&] {
        return audio_failure_event_json("test_context", aa::ErrorCode::invalid_argument,
                                        audio.failure_context());
    };
    ASSERT_TRUE(audio.ready()) << context();
    audio.on_idle_observed_for_test = [&](const auto &pending, bool flight, bool idle, const auto &report) {
        settled.push_back({pending, flight, idle, report});
    };
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(960, std::byte{1}); // 240 frames / 5 ms.
    const auto deadline = [] { return std::chrono::steady_clock::now() + std::chrono::seconds{2}; };
    ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 0, 1, 7})) << context();
    ASSERT_TRUE(audio.wait_idle(deadline())) << context();
    ASSERT_FALSE(audio.failure()) << context();
    ASSERT_EQ(settled.size(), 1U) << context();
    // A successful, fully reconciled warm-up establishes output: not startup.
    ASSERT_TRUE(settled.front().armed) << context();
    ASSERT_FALSE(settled.front().in_flight) << context();
    ASSERT_EQ(settled.front().pending, (std::array<std::size_t, 4>{})) << context();
    ASSERT_EQ(settled.front().report.stats.frames_written, 1U) << context();
    ASSERT_EQ(settled.front().report.stats.frames_output, 240U) << context();
    now.store(second_end);
    if (order == CompletionIdleOrder::settle_after_advance) {
        const std::lock_guard lock(gate->mutex);
        gate->released = false;
    }
    ASSERT_TRUE(audio.submit({{role, 2, {base + 5'000'000}}, pcm}, {role, 0, 2, 7})) << context();
    struct ReleaseSinkOnExit {
        RenderGate &gate;
        ~ReleaseSinkOnExit() { gate.release(); }
    } release_sink_on_exit{*gate};
    if (order == CompletionIdleOrder::settle_after_advance) {
        ASSERT_TRUE(gate->wait_writes(2));
        EXPECT_TRUE(audio.worker_observation_for_test().queue.in_flight);
        now.store(third_start);
        gate->release();
    }
    ASSERT_TRUE(barrier.wait_blocked()) << context();
    if (order == CompletionIdleOrder::receipt_before_settle) {
        // External telemetry cannot hold occupancy or defer the idle commitment.
        const auto state = audio.worker_observation_for_test();
        ASSERT_FALSE(state.queue.in_flight) << "external observer blocked before settlement";
        ASSERT_EQ(state.queue.queued_by_role, (std::array<std::size_t, 4>{}));
        ASSERT_TRUE(state.last_idle);
        ASSERT_TRUE(state.last_idle->eligible);
        ASSERT_TRUE(state.last_idle->idle_since);
        ASSERT_EQ(state.last_idle->idle_since->count, second_end);
    }
    {
        const std::lock_guard lock(barrier.mutex);
        ASSERT_FALSE(barrier.released) << context();
        ASSERT_EQ(barrier.observed.size(), 2U) << context();
        EXPECT_EQ(barrier.observed[1].scheduled.count, base + 5'000'000) << context();
        EXPECT_EQ(barrier.observed[1].scheduled_end.count, second_end) << context();
    }
    // Do not compare historical IdleClock values with real sink completion times.
    const bool authorized = order != CompletionIdleOrder::settle_after_advance;
    if (order == CompletionIdleOrder::settle_before_advance) {
        barrier.release();
        ASSERT_TRUE(audio.wait_idle(deadline())) << context();
        ASSERT_EQ(settled.size(), 2U) << context();
        ASSERT_TRUE(settled.back().armed) << context();
        now.store(third_start);
    } else {
        now.store(third_start);
        if (!authorized) {
            ASSERT_EQ(settled.size(), 2U) << context();
            ASSERT_TRUE(settled.back().armed) << context();
            // Receipt below has exactly the same fake time as late settlement:
            // no positive idle interval can be authorized retroactively.
        }
    }
    ASSERT_TRUE(audio.submit({{role, 3, {third_start}}, pcm}, {role, 0, 3, 7})) << context();
    const auto at_admission = audio.worker_observation_for_test();
    const auto admitted = at_admission.last_idle_close;
    ASSERT_TRUE(admitted) << context();
    EXPECT_EQ(admitted->parent.delivery, (aa::channels::AvDeliveryId{role, 0, 3, 7}));
    EXPECT_EQ(admitted->parent.received_at.count, third_start);
    EXPECT_TRUE(admitted->queue.output_established);
    ASSERT_TRUE(admitted->last_completion);
    EXPECT_EQ(admitted->last_completion->scheduled_end.count, second_end);
    ASSERT_TRUE(admitted->last_success);
    EXPECT_LE(admitted->last_completion->completed, admitted->accepted_at);
    {
        const std::lock_guard lock(barrier.mutex);
        EXPECT_EQ(admitted->last_completion->completed, barrier.observed[1].completed);
    }
    EXPECT_LE(admitted->last_success->returned_at, admitted->last_success->reconciled_at);
    EXPECT_LE(admitted->last_success->reconciled_at, admitted->accepted_at);
    EXPECT_EQ(admitted->last_success->contributors, 1U);
    EXPECT_EQ(admitted->last_success->completed, 1U);
    EXPECT_FALSE(admitted->last_success->pending);
    EXPECT_EQ(admitted->last_success->queue.queued_by_role, (std::array<std::size_t, 4>{}));
    EXPECT_FALSE(admitted->last_success->queue.in_flight);
    EXPECT_TRUE(admitted->last_success->queue.output_established);
    {
        EXPECT_FALSE(admitted->queue.in_flight);
        EXPECT_EQ(admitted->queue.queued_by_role, (std::array<std::size_t, 4>{}));
        EXPECT_EQ(admitted->disposition, authorized ? IdleDisposition::authorized
                                                  : IdleDisposition::receipt_not_after_start);
        ASSERT_TRUE(admitted->idle_since);
        EXPECT_EQ(admitted->idle_since->count, authorized ? second_end : third_start);
        EXPECT_LE(admitted->last_completion->completed, admitted->last_success->returned_at);
    }
    barrier.release();
    ASSERT_TRUE(audio.wait_idle(deadline())) << context();
    const auto before_finish = audio.failure_context();
    const auto before_finish_json = before_finish ? audio_failure_json(*before_finish) : std::string{};
    const auto idle_close_before_finish = worker_admission_json(audio.worker_observation_for_test().last_idle_close);
    const auto finished = audio.finish();
    const auto failure = audio.failure_context();
    EXPECT_EQ(failure.has_value(), before_finish.has_value());
    if (failure) { EXPECT_EQ(audio_failure_json(*failure), before_finish_json); }
    EXPECT_EQ(worker_admission_json(audio.worker_observation_for_test().last_idle_close), idle_close_before_finish);
    const auto report = audio.final_report();
    const auto output = capture.release();
    SCOPED_TRACE(output + "\n" + context());
    ASSERT_TRUE(report);
    EXPECT_FALSE(barrier.timed_out);
    EXPECT_FALSE(gate->timed_out);
    for (const auto &completion : barrier.observed) {
        const auto event = "{\"event\":\"audio_write_completed\",\"scheduled_ns\":" +
            std::to_string(completion.scheduled.count) + ",\"scheduled_end_ns\":" +
            std::to_string(completion.scheduled_end.count) + ",\"completed_ns\":" +
            std::to_string(completion.completed.count) + "}";
        const auto first = output.find(event);
        ASSERT_NE(first, std::string::npos);
        EXPECT_EQ(output.find(event, first + 1), std::string::npos);
    }
    EXPECT_EQ(report->stats.frames_silence, 0U);
    EXPECT_EQ(report->stats.frames_skipped, 0U);
    ASSERT_GE(settled.size(), 2U);
    EXPECT_FALSE(settled[1].in_flight);
    EXPECT_EQ(settled[1].report.stats.frames_written, 2U);
    EXPECT_EQ(settled[1].report.stats.frames_output, 480U);
    {
        EXPECT_TRUE(settled[1].armed);
        EXPECT_EQ(settled[1].pending, (std::array<std::size_t, 4>{}));
    }
    if (authorized) {
        EXPECT_TRUE(finished);
        EXPECT_FALSE(audio.failure());
        EXPECT_FALSE(failure);
        EXPECT_FALSE(report->render_failure);
        EXPECT_EQ(report->stats.frames_idle_skipped, 882961U);
        EXPECT_EQ(report->stats.frames_retained, 720U);
        EXPECT_EQ(report->stats.frames_output, 720U);
        EXPECT_EQ(gate->writes, 3U);
        EXPECT_EQ(barrier.observed.size(), 3U);
        EXPECT_EQ(gate->timestamps.back().count, base + (480LL + 882961LL) * 1'000'000'000 / 48000);
        const auto resume = "{\"event\":\"audio_idle_resume\",\"id\":1,\"start_ns\":" +
            std::to_string(second_end) + ",\"end_ns\":" + std::to_string(third_start) +
            ",\"idle_ns\":18395020834,\"reason\":\"all_role_idle\"}";
        EXPECT_NE(output.find(resume), std::string::npos);
        EXPECT_NE(output.find("\"frames_idle_skipped\":882961,\"frames_idle_skipped_total\":882961"),
                  std::string::npos);
        EXPECT_EQ(gate->pcm, (std::vector<std::vector<std::byte>>{pcm, pcm, pcm}));
        for (std::size_t i = 0; i < barrier.observed.size(); ++i) {
            EXPECT_EQ(barrier.observed[i].scheduled, gate->timestamps[i]);
        }
    } else {
        ASSERT_FALSE(finished);
        EXPECT_EQ(finished.error().code(), aa::ErrorCode::invalid_argument);
        ASSERT_TRUE(failure);
        EXPECT_EQ(failure->origin, AudioFailureOrigin::render_step);
        ASSERT_TRUE(failure->parent);
        EXPECT_EQ(failure->parent->delivery, (aa::channels::AvDeliveryId{role, 0, 3, 7}));
        EXPECT_EQ(failure->parent->message_sequence, 3U);
        EXPECT_EQ(failure->parent->original_scheduled.count, third_start);
        EXPECT_EQ(failure->parent->received_at.count, third_start);
        ASSERT_TRUE(failure->worker.admission);
        EXPECT_EQ(worker_admission_json(failure->worker.admission), worker_admission_json(admitted));
        EXPECT_EQ(failure->worker.queue.queued_by_role[0], 1U);
        EXPECT_FALSE(failure->worker.queue.in_flight);
        ASSERT_TRUE(failure->worker.last_completion);
        EXPECT_EQ(failure->worker.last_completion->scheduled_end.count, second_end);
        ASSERT_TRUE(failure->worker.last_success);
        EXPECT_GE(failure->worker.last_success->returned_at, failure->worker.last_completion->completed);
        ASSERT_TRUE(failure->worker.last_idle);
        EXPECT_EQ(failure->worker.last_idle->eligible, order == CompletionIdleOrder::settle_after_advance);
        ASSERT_TRUE(failure->worker.last_idle_close);
        EXPECT_EQ(failure->worker.last_idle_close->parent.delivery.sequence,
                  order == CompletionIdleOrder::receipt_before_settle ? 2U : 3U);
        ASSERT_TRUE(failure->router);
        const auto &snapshot = *failure->router;
        EXPECT_EQ(snapshot.reason, aa::audio::RenderFailureReason::single_gap);
        EXPECT_EQ(snapshot.output_sample_rate, 48000U);
        EXPECT_TRUE(snapshot.base_committed);
        EXPECT_EQ(snapshot.base_ns, base);
        EXPECT_EQ(snapshot.entry_cursor, 480U);
        EXPECT_EQ(snapshot.raw_gap, 882961U);
        EXPECT_EQ(snapshot.uncovered_gap, 882961U);
        EXPECT_EQ(snapshot.cap, 9600U);
        EXPECT_EQ(snapshot.stored_idle.count, 0U);
        EXPECT_EQ(snapshot.stored_backpressure.count, 0U);
        EXPECT_FALSE(snapshot.stored_idle_prefix);
        ASSERT_TRUE(snapshot.effective_idle);
        ASSERT_TRUE(snapshot.effective_backpressure);
        EXPECT_EQ(snapshot.effective_idle->count, 0U);
        EXPECT_EQ(snapshot.effective_idle->excluded_frames, 0U);
        EXPECT_EQ(snapshot.effective_backpressure->count, 0U);
        EXPECT_EQ(snapshot.effective_backpressure->excluded_frames, 0U);
        EXPECT_EQ(report->stats.frames_idle_skipped, 0U);
        EXPECT_EQ(report->stats.frames_output, 480U);
        EXPECT_EQ(gate->writes, 2U);
        EXPECT_EQ(barrier.observed.size(), 2U);
        EXPECT_EQ(output.find("audio_idle_resume"), std::string::npos);
        EXPECT_EQ(output.find("audio_idle_skipped"), std::string::npos);
    }
}

TEST(WiredReceiver, WorkerPostCompletionPreIdleReceiptAuthorizesExactFatalGap) {
    expect_post_completion_idle_order(CompletionIdleOrder::receipt_before_settle);
}

TEST(WiredReceiver, WorkerPostCompletionPreIdleEarlySettlementAuthorizesExactGap) {
    expect_post_completion_idle_order(CompletionIdleOrder::settle_before_advance);
}

TEST(WiredReceiver, WorkerPostCompletionPreIdleLateSettlementCannotAuthorizeExactGap) {
    expect_post_completion_idle_order(CompletionIdleOrder::settle_after_advance);
}

TEST(WiredReceiver, WorkerBlockedCompletionObserverPreservesGenerationStopAndEarlyCapacity) {
    ReceiverLogScope logging{g_running};
    for (const int scenario : {0, 1, 2, 3, 4}) {
        SCOPED_TRACE(scenario);
        auto gate = std::make_shared<RenderGate>();
        gate->release();
        CompletionBarrier barrier;
        const auto base = steady_ns() - 30'000'000'000LL;
        std::atomic<std::int64_t> now{base + 5'000'000};
        auto config = pcm_router_config();
        config.on_write_completed = [&](const auto &value) { barrier.complete(value); };
        ConcreteAudioRenderWorker audio{config, gated_router(gate),
            [&] { return aa::core::Nanoseconds{now.load()}; }};
        struct ReleaseOnExit {
            CompletionBarrier &barrier;
            ~ReleaseOnExit() { barrier.release(); }
        } release_on_exit{barrier};
        const auto deadline = [] { return std::chrono::steady_clock::now() + std::chrono::seconds{2}; };
        const auto role = aa::protocol::ChannelRole::media_audio;
        const std::vector<std::byte> pcm(960, std::byte{1});
        ASSERT_TRUE(audio.ready());
        ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 0, 1, 7}));
        ASSERT_TRUE(audio.wait_idle(deadline()));
        ASSERT_TRUE(audio.take_completion());
        now.store(base + 10'000'000);
        ASSERT_TRUE(audio.submit({{role, 2, {base + 5'000'000}}, pcm}, {role, 0, 2, 7}));
        ASSERT_TRUE(barrier.wait_blocked());
        ASSERT_TRUE(audio.wait_idle(ConcreteAudioRenderWorker::Clock::now()));
        ASSERT_TRUE(audio.take_completion());
        ASSERT_FALSE(audio.take_completion());
        if (scenario == 0) {
            audio.invalidate(role, 1);
            ASSERT_TRUE(audio.submit({{role, 2, {base + 10'000'000}}, pcm}, {role, 1, 2, 7}));
            const auto completion = audio.take_completion();
            ASSERT_TRUE(completion);
            EXPECT_EQ(*completion, (aa::channels::AvDeliveryId{role, 1, 2, 7}));
            EXPECT_FALSE(audio.take_completion());
        } else if (scenario == 1) {
            audio.stop();
            audio.invalidate(role, 1);
            EXPECT_FALSE(audio.worker_observation_for_test().last_idle->eligible);
            EXPECT_FALSE(audio.worker_observation_for_test().last_idle->idle_since);
            EXPECT_FALSE(audio.submit({{role, 2, {base + 10'000'000}}, pcm}, {role, 1, 2, 7}));
            EXPECT_FALSE(audio.take_completion());
        } else {
            now.fetch_add((scenario - 2) * 100'000'000LL);
            const std::vector<std::byte> full(19200, std::byte{1});
            ASSERT_TRUE(audio.submit({{role, 3, {base + 10'000'000}}, full}, {role, 0, 3, 7}));
            const auto admission = audio.worker_observation_for_test().last_idle_close;
            ASSERT_TRUE(admission);
            EXPECT_EQ(admission->disposition, scenario == 2 ? IdleDisposition::receipt_not_after_start
                                                          : IdleDisposition::short_interval);
            const auto first_credit = audio.take_completion();
            ASSERT_TRUE(first_credit);
            EXPECT_EQ(*first_credit, (aa::channels::AvDeliveryId{role, 0, 3, 7}));
            ASSERT_TRUE(audio.submit({{role, 4, {base + 110'000'000}}, full}, {role, 0, 4, 7}));
            EXPECT_FALSE(audio.take_completion());
            const auto rejected = audio.submit({{role, 5, {base + 210'000'000}}, pcm}, {role, 0, 5, 7});
            ASSERT_FALSE(rejected);
            EXPECT_EQ(rejected.error().code(), aa::ErrorCode::ipc_queue_full);
            ASSERT_TRUE(audio.last_queue_rejection());
            EXPECT_EQ(audio.last_queue_rejection()->bytes, 38400U);
            EXPECT_EQ(audio.worker_observation_for_test().queue.queued_by_role[0], 2U);
        }
        barrier.release();
        ASSERT_TRUE(audio.wait_idle(deadline()));
        ASSERT_TRUE(audio.finish());
        EXPECT_FALSE(barrier.timed_out);
        EXPECT_FALSE(gate->timed_out);
        EXPECT_FALSE(audio.failure());
        const auto report = audio.final_report();
        ASSERT_TRUE(report);
        EXPECT_EQ(report->stats.frames_idle_skipped, 0U);
        EXPECT_EQ(report->stats.frames_skipped, 0U);
        EXPECT_EQ(report->stats.frames_silence, 0U);
        EXPECT_EQ(barrier.observed.size(), gate->writes);
        if (scenario == 0) {
            EXPECT_EQ(gate->writes, 3U);
            EXPECT_EQ(gate->pcm, (std::vector<std::vector<std::byte>>{pcm, pcm, pcm}));
        } else if (scenario == 1) {
            EXPECT_EQ(gate->writes, 2U);
            EXPECT_FALSE(audio.worker_observation_for_test().last_idle->eligible);
        } else {
            EXPECT_EQ(report->stats.frames_retained, 10080U);
        }
    }
}

TEST(WiredReceiver, WorkerCancelledDuringLeadingSilenceCanRecoverFromSecondIdle) {
    ReceiverLogScope logging{g_running};
    StdoutCaptureGuard capture;
    auto gate = std::make_shared<RenderGate>();
    gate->release();
    const auto base = steady_ns() - 10'000'000'000LL;
    // Conservative frame rounding leaves one residual frame before the first
    // authorized range. Its abandoned prefix then fits the 200 ms gap cap.
    std::atomic<std::int64_t> now{base + 5'010'000};
    ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate),
        [&] { return aa::core::Nanoseconds{now.load()}; }};
    ASSERT_TRUE(audio.ready());
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(960, std::byte{1});
    ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 0, 1, 7}));
    ASSERT_TRUE(audio.take_completion(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    {
        const std::lock_guard lock(gate->mutex);
        gate->released = false;
    }
    now.store(base + 205'010'001);
    ASSERT_TRUE(audio.submit({{role, 2, {base + 205'010'001}}, pcm}, {role, 0, 2, 7}));
    ASSERT_TRUE(gate->wait_writes(2));
    {
        const std::lock_guard lock(gate->mutex);
        ASSERT_EQ(gate->pcm.back(), std::vector<std::byte>(4, std::byte{0}));
        EXPECT_EQ(gate->timestamps.back().count, base + 5'000'000);
    }
    // No resumed PCM has been detached: cancellation leaves authorization live
    // unless healthy worker reconciliation explicitly retires it.
    audio.invalidate(role, 1);
    EXPECT_FALSE(audio.take_completion());
    gate->release();
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_FALSE(audio.failure());
    now.store(base + 2'605'010'001);
    ASSERT_TRUE(audio.submit({{role, 3, {base + 2'605'000'000}}, pcm}, {role, 1, 3, 7}));
    const auto completion = audio.take_completion(std::chrono::steady_clock::now() + std::chrono::seconds{2});
    ASSERT_TRUE(completion);
    EXPECT_EQ(*completion, (aa::channels::AvDeliveryId{role, 1, 3, 7}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_FALSE(audio.failure());
    ASSERT_TRUE(audio.finish());
    const auto report = audio.final_report();
    ASSERT_TRUE(report);
    EXPECT_GT(report->stats.frames_idle_skipped, 115000U);
    EXPECT_EQ(report->stats.frames_skipped, 0U);
    EXPECT_EQ(report->stats.frames_retained, 480U);
    EXPECT_EQ(report->stats.frames_silence, 9601U);
    EXPECT_EQ(gate->timestamps.back().count, base + 2'605'000'000);
    EXPECT_FALSE(gate->timed_out);
    const auto output = capture.release();
    EXPECT_NE(output.find("\"event\":\"audio_idle_resume\",\"id\":2"), std::string::npos) << output;
    EXPECT_NE(output.find("\"event\":\"audio_idle_skipped\",\"id\":2"), std::string::npos) << output;
}

TEST(WiredReceiver, WorkerIdleSkipCorrelationTotalsSlicesAroundBackpressure) {
    ReceiverLogScope logging{g_running};
    StdoutCaptureGuard capture;
    auto gate = std::make_shared<RenderGate>();
    gate->release();
    const auto base = steady_ns() - 10'000'000'000LL;
    std::atomic<std::int64_t> now{base + 5'000'000};
    ConcreteAudioRenderWorker audio{pcm_router_config(), [&](const auto &config) {
        auto opened = gated_router(gate)(config);
        if (opened) {
            opened.value().note_backpressure({base + 805'000'000}, {base + 1'005'000'000});
        }
        return opened;
    }, [&] { return aa::core::Nanoseconds{now.load()}; }};
    ASSERT_TRUE(audio.ready());
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(960, std::byte{1});
    ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 0, 1, 7}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    now.store(base + 2'405'000'000);
    ASSERT_TRUE(audio.submit({{role, 2, {base + 2'405'000'000}}, pcm}, {role, 0, 2, 7}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_FALSE(audio.failure());
    ASSERT_TRUE(audio.finish());
    const auto report = audio.final_report();
    ASSERT_TRUE(report);
    EXPECT_EQ(report->stats.frames_idle_skipped, 105600U);
    EXPECT_EQ(report->stats.frames_skipped, 9600U);
    EXPECT_EQ(report->stats.frames_silence, 0U);
    EXPECT_EQ(report->stats.frames_written, 2U);
    const auto output = capture.release();
    const auto event = output.find("{\"event\":\"audio_idle_skipped\",\"id\":1,\"frames_idle_skipped\":105600,"
                                   "\"frames_idle_skipped_total\":105600}");
    ASSERT_NE(event, std::string::npos) << output;
    EXPECT_EQ(output.find("\"event\":\"audio_idle_skipped\"", event + 2), std::string::npos) << output;
}

TEST(WiredReceiver, WorkerOtherRolePendingWithoutInflightWriteDoesNotArmIdle) {
    ReceiverLogScope logging{g_running};
    auto gate = std::make_shared<RenderGate>();
    const auto base = steady_ns() - 10'000'000'000LL;
    std::atomic<std::int64_t> now{base + 5'000'000};
    ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate),
        [&] { return aa::core::Nanoseconds{now.load()}; }};
    ASSERT_TRUE(audio.ready());
    bool observed = false;
    bool in_flight = true;
    bool idle_armed = true;
    audio.on_idle_observed_for_test = [&](const auto &pending, bool flight, bool idle, const auto &) {
        if (pending[0] == 0 && pending[1] != 0) {
            observed = true;
            in_flight = flight;
            idle_armed = idle;
        }
    };
    const auto media = aa::protocol::ChannelRole::media_audio;
    const auto voice = aa::protocol::ChannelRole::guidance_audio;
    const std::vector<std::byte> pcm(960, std::byte{1});
    const std::vector<std::byte> voice_pcm(160, std::byte{1});
    ASSERT_TRUE(audio.submit({{media, 1, {base}}, pcm}, {media, 0, 1, 7}));
    ASSERT_TRUE(gate->wait_writes(1));
    ASSERT_TRUE(audio.submit({{voice, 2, {base + 5'000'000}}, voice_pcm}, {voice, 0, 2, 7}));
    now.fetch_add(2'400'000'000);
    gate->release();
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(audio.finish());
    EXPECT_TRUE(observed);
    EXPECT_FALSE(in_flight);
    EXPECT_FALSE(idle_armed);
    EXPECT_EQ(audio.final_report()->stats.frames_idle_skipped, 0U);
}

TEST(WiredReceiver, RealGstSameWorkerIdlePauseResumesWithoutReopeningRoute) {
    ReceiverLogScope logging{g_running};
    StdoutCaptureGuard capture;
    struct RealRoute {
        RealRoute() { runtime_sink_request_override = real_runtime_sink; }
        ~RealRoute() { runtime_sink_request_override = {}; }
    } real_route;
    std::mutex progress_mutex;
    std::vector<aa::audio::SinkWriteProgress> progress;
    std::vector<aa::audio::WriteCompletion> completions;
    std::vector<aa::audio::RouterReport> settled;
    auto config = pcm_router_config();
    config.on_sink_write_progress = [&](const auto &value) {
        const std::lock_guard lock(progress_mutex);
        if (!value.readiness_probe && value.completed_at) { progress.push_back(value); }
    };
    config.on_write_completed = [&, log = config.on_write_completed](const auto &value) {
        log(value);
        const std::lock_guard lock(progress_mutex);
        completions.push_back(value);
    };
    std::size_t opens = 0;
    ConcreteAudioRenderWorker audio{config, [&](const auto &value) {
        ++opens;
        return aa::audio::AudioRouter::open(value);
    }};
    const auto ready = audio.ready();
    if (!ready) {
        (void)audio.finish();
        const auto report = audio.final_report();
        GTEST_SKIP() << "AudioRouter::open real route unavailable: " << aa::to_string(ready.error().code())
                     << " cause=" << (report ? report->sink.cause : "open returned no router")
                     << " output=" << capture.release();
    }
    audio.on_idle_observed_for_test = [&](const auto &pending, bool flight, bool idle, const auto &report) {
        if (!flight && idle && std::all_of(pending.begin(), pending.end(), [](auto count) { return count == 0; })) {
            settled.push_back(report);
        }
    };
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(960, std::byte{1});
    const auto first = steady_ns();
    ASSERT_TRUE(audio.submit({{role, 1, {first}}, pcm}, {role, 0, 1, 7}));
    const auto first_credit = audio.take_completion(std::chrono::steady_clock::now() + std::chrono::seconds{2});
    ASSERT_TRUE(first_credit);
    EXPECT_EQ(*first_credit, (aa::channels::AvDeliveryId{role, 0, 1, 7}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_EQ(settled.size(), 1U);
    const auto before = settled.front();
    std::mutex pause_mutex;
    std::condition_variable pause_cv;
    std::unique_lock pause_lock(pause_mutex);
    const auto pause_start = std::chrono::steady_clock::now();
    pause_cv.wait_until(pause_lock, pause_start + std::chrono::milliseconds{300}, [] { return false; });
    const auto pause_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - pause_start).count();
    // Whole milliseconds are exact 48-frame boundaries on the original timeline.
    const auto second = first + ((steady_ns() - first + 999'999) / 1'000'000) * 1'000'000;
    ASSERT_TRUE(audio.submit({{role, 2, {second}}, pcm}, {role, 0, 2, 7}));
    const auto second_credit = audio.take_completion(std::chrono::steady_clock::now() + std::chrono::seconds{2});
    ASSERT_TRUE(second_credit);
    EXPECT_EQ(*second_credit, (aa::channels::AvDeliveryId{role, 0, 2, 7}));
    const auto third = second + 5'000'000;
    ASSERT_TRUE(audio.submit({{role, 3, {third}}, pcm}, {role, 0, 3, 7}));
    const auto third_credit = audio.take_completion(std::chrono::steady_clock::now() + std::chrono::seconds{2});
    ASSERT_TRUE(third_credit);
    EXPECT_EQ(*third_credit, (aa::channels::AvDeliveryId{role, 0, 3, 7}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    EXPECT_FALSE(audio.take_completion());
    ASSERT_FALSE(audio.failure());
    ASSERT_TRUE(audio.finish());
    const auto report = audio.final_report();
    ASSERT_TRUE(report);
    const auto output = capture.release();
    EXPECT_EQ(opens, 1U);
    // Contiguous parents may settle together or briefly idle between writes.
    ASSERT_GE(settled.size(), 2U);
    EXPECT_LE(settled.size(), 3U);
    EXPECT_EQ(report->state, aa::audio::RouterState::active);
    EXPECT_EQ(report->sink.selected_element, before.sink.selected_element);
    EXPECT_EQ(report->stats.chunks_submitted, 3U);
    EXPECT_GT(report->stats.frames_idle_skipped, 0U);
    EXPECT_GT(settled.back().render_barrier.chain_return_buffers, before.render_barrier.chain_return_buffers);
    EXPECT_GT(settled.back().render_barrier.handoff_events, before.render_barrier.handoff_events);
    EXPECT_LE(report->stats.frames_silence, 4800U);
    EXPECT_LE(report->stats.frames_output, 5520U);
    EXPECT_EQ(report->stats.frames_retained, 720U);
    EXPECT_EQ(output.find("invalid_argument"), std::string::npos) << output;
    EXPECT_EQ(output.find("reconnect"), std::string::npos) << output;
    for (const auto event : {"\"event\":\"audio_idle_resume\"", "\"event\":\"audio_idle_skipped\"",
                             "\"reason\":\"all_role_idle\""}) {
        const auto position = output.find(event);
        ASSERT_NE(position, std::string::npos) << output;
        EXPECT_EQ(output.find(event, position + 1), std::string::npos) << output;
    }
    EXPECT_NE(output.find("{\"event\":\"audio_idle_resume\",\"id\":1,"), std::string::npos) << output;
    EXPECT_NE(output.find("{\"event\":\"audio_idle_skipped\",\"id\":1,\"frames_idle_skipped\":" +
                          std::to_string(report->stats.frames_idle_skipped) + ",\"frames_idle_skipped_total\":" +
                          std::to_string(report->stats.frames_idle_skipped) + "}"), std::string::npos) << output;
    const std::lock_guard progress_lock(progress_mutex);
    for (const auto scheduled : {first, second, third}) {
        EXPECT_EQ(std::count_if(completions.begin(), completions.end(), [&](const auto &value) {
            return value.scheduled.count == scheduled && value.scheduled_end.count == scheduled + 5'000'000;
        }), 1) << "expected exactly one original scheduled burst " << scheduled;
    }
    ASSERT_FALSE(progress.empty());
    ASSERT_TRUE(progress.front().pts_origin_ns);
    for (const auto &value : progress) {
        EXPECT_EQ(value.pts_origin_ns, progress.front().pts_origin_ns);
        EXPECT_TRUE(value.handoff_at);
        EXPECT_TRUE(value.marker_at);
        EXPECT_FALSE(value.outcome);
    }
    std::cout << "IDLE_RESUME_GST sink=" << report->sink.selected_element << " opens=" << opens
              << " pause_ns=" << pause_ns << " idle_skipped=" << report->stats.frames_idle_skipped
              << " silence_frames=" << report->stats.frames_silence
              << " output_frames=" << report->stats.frames_output
              << " marker=" << before.render_barrier.chain_return_buffers << "->"
              << settled.back().render_barrier.chain_return_buffers
              << " handoff=" << before.render_barrier.handoff_events << "->"
              << settled.back().render_barrier.handoff_events
              << " pts_origin_ns=" << *progress.front().pts_origin_ns << '\n';
}

TEST(WiredReceiver, WorkerShortSettledIdleNeverAuthorizesLongScheduledGap) {
    ReceiverLogScope logging{g_running};
    for (const auto duration : {0LL, 100'000'000LL, 200'000'000LL}) {
        SCOPED_TRACE(duration);
        StdoutCaptureGuard capture;
        auto gate = std::make_shared<RenderGate>();
        gate->release();
        const auto base = steady_ns() - 10'000'000'000LL;
        std::atomic<std::int64_t> now{base + 5'000'000};
        ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate),
            [&] { return aa::core::Nanoseconds{now.load()}; }};
        ASSERT_TRUE(audio.ready());
        const auto role = aa::protocol::ChannelRole::media_audio;
        const std::vector<std::byte> pcm(960, std::byte{1});
        ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 0, 1, 7}));
        ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        now.fetch_add(duration);
        ASSERT_TRUE(audio.submit({{role, 2, {base + 2'405'000'000}}, pcm}, {role, 0, 2, 7}));
        ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        ASSERT_TRUE(audio.failure());
        EXPECT_EQ(audio.failure()->code(), aa::ErrorCode::invalid_argument);
        EXPECT_FALSE(audio.finish());
        EXPECT_EQ(audio.final_report()->stats.frames_idle_skipped, 0U);
        const auto output = capture.release();
        EXPECT_EQ(output.find("audio_idle_resume"), std::string::npos) << output;
        EXPECT_EQ(output.find("audio_idle_skipped"), std::string::npos) << output;
    }
}

// Issue 60 v19 7200 FAIL shape: one write held ~500 ms past its scheduled end
// under host load, the phone's next chunk resumed ~500 ms later, and the
// receiver latched single_gap. Time spent blocked inside our own sink write is
// observed, unrenderable time: it is skipped and logged, not fatal.
TEST(WiredReceiver, WorkerSinkStallPastScheduledEndSkipsElapsedGapInsteadOfFailing) {
    ReceiverLogScope logging{g_running};
    StdoutCaptureGuard capture;
    auto gate = std::make_shared<RenderGate>();
    gate->first_write_delay = std::chrono::milliseconds{600};
    ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate)};
    ASSERT_TRUE(audio.ready());
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(960, std::byte{1});
    const auto base = steady_ns();
    ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 0, 1, 7}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_FALSE(audio.failure());
    const auto resumed = steady_ns() + 5'000'000;
    ASSERT_TRUE(audio.submit({{role, 2, {resumed}}, pcm}, {role, 0, 2, 7}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_FALSE(audio.failure());
    ASSERT_TRUE(audio.submit({{role, 3, {resumed + 5'000'000}}, pcm}, {role, 0, 3, 7}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    const bool failed = audio.failure().has_value();
    const bool finished = audio.finish().has_value();
    const auto output = capture.release();
    ASSERT_FALSE(failed) << output;
    ASSERT_TRUE(finished) << output;
    const auto report = audio.final_report();
    ASSERT_TRUE(report);
    EXPECT_FALSE(report->render_failure);
    // >=550 ms of the ~600 ms stall is skipped and counted, never synthesized.
    EXPECT_GE(report->stats.frames_skipped, 26'400U);
    EXPECT_LT(report->stats.frames_silence, 9'600U);
    EXPECT_EQ(report->stats.frames_retained, 720U);
    EXPECT_EQ(report->stats.chunks_submitted, 3U);
    EXPECT_GE(report->stats.max_completion_skew.count, 550'000'000);
    const auto continued = std::find_if(gate->timestamps.begin(), gate->timestamps.end(), [&](const auto &stamp) {
        return stamp.count >= resumed + 5'000'000 - 20'834 && stamp.count <= resumed + 5'000'000;
    });
    ASSERT_NE(continued, gate->timestamps.end());
    EXPECT_EQ(gate->pcm[static_cast<std::size_t>(continued - gate->timestamps.begin())], pcm);
    const auto event = output.find("{\"event\":\"audio_sink_stall\",");
    ASSERT_NE(event, std::string::npos) << output;
    EXPECT_EQ(output.find("\"event\":\"audio_sink_stall\"", event + 2), std::string::npos) << output;
}

TEST(WiredReceiver, WorkerStartupInflightAndOtherRolePendingNeverAuthorizeIdle) {
    ReceiverLogScope logging{g_running};
    for (const int scenario : {0, 1, 2}) {
        SCOPED_TRACE(scenario);
        StdoutCaptureGuard capture;
        auto gate = std::make_shared<RenderGate>();
        const auto base = steady_ns() - 10'000'000'000LL;
        std::atomic<std::int64_t> now{base + 5'000'000};
        ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate),
            [&] { return aa::core::Nanoseconds{now.load()}; }};
        ASSERT_TRUE(audio.ready());
        const auto role = aa::protocol::ChannelRole::media_audio;
        const auto voice = aa::protocol::ChannelRole::guidance_audio;
        const std::vector<std::byte> pcm(960, std::byte{1});
        if (scenario == 0) { now.fetch_add(2'400'000'000); }
        ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 0, 1, 7}));
        ASSERT_TRUE(gate->wait_writes(1));
        if (scenario == 2) {
            const std::vector<std::byte> voice_pcm(160, std::byte{1});
            ASSERT_TRUE(audio.submit({{voice, 3, {base + 5'000'000}}, voice_pcm}, {voice, 0, 3, 7}));
            audio.invalidate(role, 1);
        } else if (scenario == 1) {
            // Even an empty queue is not settled while its detached write remains.
            audio.invalidate(role, 1);
        }
        now.store(base + 2'405'000'000);
        ASSERT_TRUE(audio.submit({{role, 2, {base + 2'405'000'000}}, pcm},
                                  {role, scenario == 0 ? 0U : 1U, 2, 7}));
        gate->release();
        ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        ASSERT_TRUE(audio.failure());
        EXPECT_EQ(audio.failure()->code(), aa::ErrorCode::invalid_argument);
        EXPECT_FALSE(audio.finish());
        EXPECT_EQ(audio.final_report()->stats.frames_idle_skipped, 0U);
        const auto output = capture.release();
        EXPECT_EQ(output.find("audio_idle_resume"), std::string::npos) << output;
        EXPECT_EQ(output.find("audio_idle_skipped"), std::string::npos) << output;
    }
}

TEST(WiredReceiver, WorkerPrematureReceiptCannotBecomeIdleDuringDelayedProcessing) {
    ReceiverLogScope logging{g_running};
    StdoutCaptureGuard capture;
    auto gate = std::make_shared<RenderGate>();
    const auto base = steady_ns() - 10'000'000'000LL;
    std::atomic<std::int64_t> now{base + 5'000'000};
    ConcreteAudioRenderWorker *worker = nullptr;
    const auto owner = std::this_thread::get_id();
    bool delay_receipt = false;
    ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate), [&] {
        const aa::core::Nanoseconds receipt{now.load()};
        if (std::this_thread::get_id() == owner && delay_receipt) {
            // Capture receipt before settlement, then let reconciliation finish
            // before returning it to submit's owner-side processing.
            delay_receipt = false;
            gate->release();
            EXPECT_TRUE(worker->wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
            now.fetch_add(2'400'000'000);
        }
        return receipt;
    }};
    worker = &audio;
    ASSERT_TRUE(audio.ready());
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(960, std::byte{1});
    ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 0, 1, 7}));
    ASSERT_TRUE(gate->wait_writes(1));
    delay_receipt = true;
    ASSERT_TRUE(audio.submit({{role, 2, {base + 2'405'000'000}}, pcm}, {role, 0, 2, 7}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(audio.failure());
    EXPECT_EQ(audio.failure()->code(), aa::ErrorCode::invalid_argument);
    EXPECT_FALSE(audio.finish());
    EXPECT_EQ(audio.final_report()->stats.frames_idle_skipped, 0U);
    const auto output = capture.release();
    EXPECT_EQ(output.find("audio_idle_resume"), std::string::npos) << output;
}

TEST(WiredReceiver, WorkerSettledIdleCannotReviveStoppedOrFailedRoute) {
    ReceiverLogScope logging{g_running};
    for (const bool fail : {false, true}) {
        StdoutCaptureGuard capture;
        auto gate = std::make_shared<RenderGate>();
        gate->fail_write = fail;
        gate->release();
        const auto base = steady_ns() - 10'000'000'000LL;
        std::atomic<std::int64_t> now{base + 5'000'000};
        ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate),
            [&] { return aa::core::Nanoseconds{now.load()}; }};
        ASSERT_TRUE(audio.ready());
        const auto role = aa::protocol::ChannelRole::media_audio;
        const std::vector<std::byte> pcm(960, std::byte{1});
        ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 0, 1, 7}));
        ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        if (!fail) { audio.stop(); }
        now.fetch_add(2'400'000'000);
        audio.invalidate(role, 1);
        const auto rejected = audio.submit({{role, 2, {base + 2'405'000'000}}, pcm}, {role, 1, 2, 7});
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code(), fail ? aa::ErrorCode::audio_sink_unavailable : aa::ErrorCode::cancelled);
        EXPECT_EQ(audio.finish().has_value(), !fail);
        EXPECT_EQ(audio.final_report()->stats.frames_idle_skipped, 0U);
        const auto output = capture.release();
        EXPECT_EQ(output.find("audio_idle_resume"), std::string::npos) << output;
        EXPECT_EQ(output.find("audio_idle_skipped"), std::string::npos) << output;
    }
}

TEST(WiredReceiver, WorkerCapacityChargesReservationsUntilRenderAndRejectsOverflowWithoutCredit) {
    ReceiverLogScope logging{g_running};
    const auto role = aa::protocol::ChannelRole::media_audio;
    const auto voice = aa::protocol::ChannelRole::guidance_audio;
    for (const int scenario : {0, 1, 2}) {
        SCOPED_TRACE(scenario);
        auto gate = std::make_shared<RenderGate>();
        ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate)};
        ASSERT_TRUE(audio.ready());
        const std::vector<std::byte> first(scenario == 0 ? 4U : 19200U, std::byte{1});
        const auto base = steady_ns();
        ASSERT_TRUE(audio.submit({{role, 7, {base}}, first}, {role, 0, 7, 3}));
        ASSERT_TRUE(gate->wait_writes(1));
        const std::vector<std::byte> rest(scenario == 0 ? 4U : scenario == 1 ? 19200U : 6400U,
                                          std::byte{1});
        const int count = scenario == 0 ? 3 : 1;
        std::vector<aa::channels::AvDeliveryId> admitted{{role, 0, 7, 3}};
        for (int i = 0; i < count; ++i) {
            const auto second_role = scenario != 2 ? role : i == 0 ? voice
                : aa::protocol::ChannelRole::system_audio;
            const aa::channels::AvDeliveryId id{second_role, 0,
                                                static_cast<std::uint64_t>(8 + i), 3};
            ASSERT_TRUE(audio.submit({{second_role, id.sequence,
                                      {base + (scenario == 0 ? (i + 1) * 20'834LL
                                                                : 100'000'000LL + i * 50'000'000LL)}}, rest},
                                     id));
            admitted.push_back(id);
        }
        const auto full_role = scenario == 2 ? voice : role;
        const aa::channels::AvDeliveryId overflow_id{full_role, 0, 99, 3};
        const auto rejected = audio.submit({{full_role, 99, {base}}, {rest.data(), 1}}, overflow_id);
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code(), aa::ErrorCode::ipc_queue_full);
        const auto rejection = audio.last_queue_rejection();
        ASSERT_TRUE(rejection);
        EXPECT_EQ(rejection->source, aa::diagnostics::QueueSource::audio_worker);
        EXPECT_EQ(rejection->reason, scenario == 0 ? aa::diagnostics::QueueReason::slots
            : aa::diagnostics::QueueReason::bytes);
        EXPECT_EQ(rejection->items, admitted.size());
        EXPECT_EQ(rejection->capacity, 12U);
        EXPECT_EQ(rejection->byte_capacity, 51200U);
        EXPECT_EQ(rejection->unit_capacity, 115200U);
        EXPECT_EQ(rejection->bytes, scenario == 0 ? 16U : scenario == 1 ? 38400U : 25600U);
        EXPECT_EQ(rejection->units, scenario == 0 ? 16U : scenario == 1 ? 38400U : 57600U);
        EXPECT_EQ(rejection->requested_bytes, 1U);
        EXPECT_EQ(rejection->requested_units, scenario == 2 ? 12U : 4U);
        const auto charged = audio.rejection_snapshot();
        ASSERT_TRUE(charged);
        ASSERT_TRUE(charged->active);
        EXPECT_EQ(charged->active->delivery, admitted.front());
        EXPECT_FALSE(charged->active->write);
        std::array<std::size_t, 4> expected_counts{};
        for (const auto &id : admitted) {
            ++expected_counts[id.role == role ? 0U : id.role == voice ? 1U : 2U];
        }
        EXPECT_EQ(charged->occupied_by_role, expected_counts);
        const std::size_t credited = scenario == 0 ? 3 : 1;
        EXPECT_EQ(charged->pending_completions_by_role,
                  (std::array<std::size_t, 4>{credited, 0, 0, 0}));
        for (std::size_t i = 0; i < credited; ++i) {
            const auto completion = audio.take_completion(std::chrono::steady_clock::now() +
                                                          std::chrono::seconds{2});
            ASSERT_TRUE(completion);
            EXPECT_EQ(*completion, admitted[i]);
        }
        EXPECT_FALSE(audio.take_completion(std::chrono::steady_clock::now()));
        // Reservations stay charged until render: taking every completion did
        // not free capacity.
        EXPECT_TRUE(audio.outstanding());
        const auto still_full = audio.submit({{full_role, 99, {base}}, {rest.data(), 1}}, overflow_id);
        ASSERT_FALSE(still_full);
        EXPECT_EQ(still_full.error().code(), aa::ErrorCode::ipc_queue_full);
        const auto frozen = audio.rejection_snapshot();
        ASSERT_TRUE(frozen);
        EXPECT_EQ(frozen->pending_completions_by_role, (std::array<std::size_t, 4>{0, 0, 0, 0}));
        gate->release();
        ASSERT_TRUE(gate->wait_writes(static_cast<std::size_t>(count + 1)));
        ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        EXPECT_EQ(audio.rejection_snapshot(), frozen);
        // Every withheld credit is granted exactly once after the renders.
        std::vector<aa::channels::AvDeliveryId> granted;
        while (const auto completion = audio.take_completion()) { granted.push_back(*completion); }
        ASSERT_EQ(granted.size(), admitted.size() - credited);
        for (const auto &id : granted) {
            EXPECT_NE(std::find(admitted.begin() + static_cast<std::ptrdiff_t>(credited),
                                admitted.end(), id), admitted.end());
        }
        const std::int64_t tail_stamp = scenario == 0 ? 4 * 20'834LL
            : scenario == 1 ? 200'000'000LL : 250'000'000LL;
        const std::vector<std::byte> tail_pcm(1600, std::byte{1});
        const auto after_render =
            audio.submit({{full_role, 99, {base + tail_stamp}}, tail_pcm}, overflow_id);
        ASSERT_TRUE(after_render);
        EXPECT_FALSE(audio.last_queue_rejection());
        EXPECT_FALSE(audio.current_rejection_snapshot());
        EXPECT_EQ(audio.rejection_snapshot(), frozen);
        const auto tail = audio.take_completion(std::chrono::steady_clock::now() +
                                                std::chrono::milliseconds{200});
        ASSERT_TRUE(tail);
        EXPECT_EQ(*tail, overflow_id);
        EXPECT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
        EXPECT_FALSE(audio.outstanding());
        EXPECT_TRUE(audio.finish());
        EXPECT_EQ(audio.rejection_snapshot(), frozen);
        EXPECT_FALSE(gate->timed_out);
    }
}

TEST(WiredReceiver, WorkerCopiesQueuedPcmAndHeaderAndDiscardsOnlyStaleRole) {
    ReceiverLogScope logging{g_running};
    auto gate = std::make_shared<RenderGate>();
    ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate)};
    ASSERT_TRUE(audio.ready());
    using aa::protocol::ChannelRole;
    const auto base = steady_ns();
    auto pcm = wiredtest::pcm_unit();
    ASSERT_TRUE(audio.submit({{ChannelRole::media_audio, 19, {base}}, pcm},
                             {ChannelRole::media_audio, 0, 19, -91}));
    ASSERT_TRUE(gate->wait_writes(1));
    const auto original = pcm;
    ASSERT_TRUE(audio.submit({{ChannelRole::media_audio, 20, {base + 20'000'000}}, pcm},
                             {ChannelRole::media_audio, 0, 20, -91}));
    pcm.assign(pcm.size(), std::byte{0});
    pcm.clear();
    pcm.shrink_to_fit();
    audio.invalidate(ChannelRole::guidance_audio, 3);
    {
        std::unique_lock lock(gate->mutex);
        gate->cv.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::milliseconds{40},
                             [] { return false; });
    }
    gate->release();
    auto first = audio.take_completion(std::chrono::steady_clock::now() + std::chrono::seconds{2});
    auto second = audio.take_completion(std::chrono::steady_clock::now() + std::chrono::seconds{2});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(*first, (aa::channels::AvDeliveryId{ChannelRole::media_audio, 0, 19, -91}));
    EXPECT_EQ(*second, (aa::channels::AvDeliveryId{ChannelRole::media_audio, 0, 20, -91}));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    ASSERT_TRUE(audio.finish());
    ASSERT_EQ(gate->pcm.size(), 2U);
    std::vector<std::byte> second_pcm;
    for (std::size_t i = 1; i < gate->pcm.size(); ++i) {
        second_pcm.insert(second_pcm.end(), gate->pcm[i].begin(), gate->pcm[i].end());
    }
    EXPECT_EQ(second_pcm, original);
    EXPECT_EQ(gate->timestamps[1].count, base + 20'000'000);
    ASSERT_TRUE(audio.final_report());
    EXPECT_GE(audio.final_report()->stats.last_path_latency.count, 40'000'000);
}

TEST(WiredReceiver, WorkerInvalidationReleasesUndrainedReservationsWithoutEvictionOfNewGeneration) {
    ReceiverLogScope logging{g_running};
    auto gate = std::make_shared<RenderGate>();
    ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate)};
    ASSERT_TRUE(audio.ready());
    const auto role = aa::protocol::ChannelRole::media_audio;
    const std::vector<std::byte> pcm(4, std::byte{1});
    const auto base = steady_ns();
    for (std::uint64_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(audio.submit({{role, i, {base + static_cast<std::int64_t>(i) * 20'834}}, pcm},
                                 {role, 0, i, 1}));
    }
    ASSERT_TRUE(gate->wait_writes(1));
    // Invalidation releases all parent storage; only the detached step survives.
    audio.invalidate(role, 1);
    EXPECT_FALSE(audio.take_completion(std::chrono::steady_clock::now()));
    for (std::uint64_t i = 4; i < 7; ++i) {
        ASSERT_TRUE(audio.submit({{role, i, {base + static_cast<std::int64_t>(i) * 20'834}}, pcm},
                                 {role, 1, i, 1}));
    }
    audio.invalidate(role, 1);
    for (std::uint64_t i = 4; i < 7; ++i) {
        const auto credited = audio.take_completion();
        ASSERT_TRUE(credited);
        EXPECT_EQ(*credited, (aa::channels::AvDeliveryId{role, 1, i, 1}));
    }
    EXPECT_FALSE(audio.take_completion(std::chrono::steady_clock::now()));
    gate->release();
    ASSERT_TRUE(gate->wait_writes(4));
    ASSERT_TRUE(audio.wait_idle(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
    EXPECT_FALSE(audio.take_completion(std::chrono::steady_clock::now()));
    EXPECT_FALSE(audio.outstanding());
    EXPECT_TRUE(audio.finish());
    EXPECT_FALSE(gate->timed_out);
}

TEST(WiredReceiver, WorkerTerminalStopDiscardsQueuedTailAndFinalDrainFailureIsLatchedOnce) {
    ReceiverLogScope logging{g_running};
    for (const bool fail_drain : {false, true}) {
        auto gate = std::make_shared<RenderGate>();
        gate->fail_drain = fail_drain;
        ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate)};
        ASSERT_TRUE(audio.ready());
        const auto role = aa::protocol::ChannelRole::media_audio;
        const auto pcm = wiredtest::pcm_unit();
        const auto base = steady_ns();
        ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 0, 1, 1}));
        ASSERT_TRUE(gate->wait_writes(1));
        ASSERT_TRUE(audio.submit({{role, 2, {base + 20'000'000}}, pcm}, {role, 0, 2, 1}));
        audio.stop();
        EXPECT_FALSE(audio.submit({{role, 3, {base}}, pcm}, {role, 0, 3, 1}));
        gate->release();
        EXPECT_EQ(audio.finish().has_value(), !fail_drain);
        EXPECT_EQ(audio.finish().has_value(), !fail_drain);
        EXPECT_FALSE(audio.take_completion());
        EXPECT_EQ(gate->writes, 1U);
        EXPECT_EQ(gate->drains, 1U);
        EXPECT_EQ(gate->destroyed, 1U);
        EXPECT_FALSE(gate->timed_out);
        EXPECT_TRUE(audio.final_report());
    }
}

TEST(WiredReceiver, WorkerOpenFailureJoinsAndPreservesOriginalError) {
    ReceiverLogScope logging{g_running};
    std::thread::id open_thread;
    const auto owner = std::this_thread::get_id();
    ConcreteAudioRenderWorker audio{pcm_router_config(), [&](const auto &)
        -> aa::core::Result<aa::audio::AudioRouter> {
        open_thread = std::this_thread::get_id();
        return aa::Error{aa::ErrorCode::audio_decode_failed};
    }};
    const auto ready = audio.ready();
    ASSERT_FALSE(ready);
    EXPECT_EQ(ready.error().code(), aa::ErrorCode::audio_decode_failed);
    EXPECT_FALSE(audio.finish());
    EXPECT_FALSE(audio.finish());
    EXPECT_NE(open_thread, owner);
    EXPECT_FALSE(audio.final_report());
}

TEST(WiredReceiver, WorkerTimeoutHonoringAdapterStopsWithoutRenderingQueuedTail) {
    ReceiverLogScope logging{g_running};
    auto gate = std::make_shared<RenderGate>();
    gate->write_timeout = std::chrono::milliseconds{30};
    ConcreteAudioRenderWorker audio{pcm_router_config(), gated_router(gate)};
    ASSERT_TRUE(audio.ready());
    const auto role = aa::protocol::ChannelRole::media_audio;
    const auto pcm = wiredtest::pcm_unit();
    const auto base = steady_ns();
    ASSERT_TRUE(audio.submit({{role, 1, {base}}, pcm}, {role, 0, 1, 1}));
    ASSERT_TRUE(gate->wait_writes(1));
    for (std::uint64_t i = 2; i <= 4; ++i) {
        ASSERT_TRUE(audio.submit({{role, i, {base + static_cast<std::int64_t>(i) * 20'000'000}}, pcm},
                                 {role, 0, i, 1}));
    }
    const auto started = std::chrono::steady_clock::now();
    audio.stop();
    const auto result = audio.finish();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), aa::ErrorCode::audio_sink_unavailable);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds{500});
    EXPECT_TRUE(gate->timed_out);
    EXPECT_EQ(gate->writes, 1U);
    EXPECT_EQ(gate->destroyed, 1U);
    EXPECT_FALSE(audio.take_completion());
}

TEST(WiredReceiver, ForgetPhoneWhileWaitingForVideoConsumerCompletesWithoutDeadlock) {
    namespace bus = aa::ipc::dbus_adapter;
    namespace faults = aa::trust::test::faults;
    char directory[] = "/tmp/wired-gate-revoke-XXXXXX";
    const std::filesystem::path root{::mkdtemp(directory)};
    const auto path = root / "approved.json";
    aa::trust::ApprovedPhoneStore initial{path};
    ASSERT_TRUE(initial.load().has_value());
    const aa::trust::WiredIdentity identity{
        "04e8", "6860", "SYNTHETIC", "TEST", "TEST", "SYNTHETIC", "1-2.1"};
    const auto approved = initial.approve(aa::trust::TransportIdentity{identity});
    ASSERT_TRUE(approved.has_value());
    runtime_phone = aa::app::wired_phone_identity(identity).value();
    runtime_recovers = false;
    runtime_quiet = false;
    runtime_connects.store(0);
    runtime_ciphers.store(0);
    g_running.store(true);
    auto state = std::make_shared<AdmissionState>();
    admission_link_state = state;
    std::vector<std::string> args{"receiver", "--port", "1-2.1", "--store", path.string(),
        "--video-socket", (root / "video").string(), "--input-socket", (root / "input").string(),
        "--duration-ms", "8000", "--require-video-consumer"};
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto &arg : args) {
        argv.push_back(arg.data());
    }
    std::vector<std::thread::id> mutations;
    faults::arm(faults::Fault::none);
    faults::set_observer([&] { mutations.push_back(std::this_thread::get_id()); });
    StdoutCaptureGuard capture;
    int code = -1;
    std::thread::id dispatcher;
    ReceiverRunGuard guard;
    guard.worker = std::thread([&] {
        dispatcher = std::this_thread::get_id();
        code = wired_receiver_main(static_cast<int>(argv.size()), argv.data());
    });
    std::atomic<bool> watchdog_fired{false};
    std::jthread watchdog([&](const std::stop_token &stop) {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::unique_lock lock(mutex);
        cv.wait_for(lock, stop, std::chrono::seconds{5}, [] { return false; });
        if (!stop.stop_requested()) {
            watchdog_fired.store(true);
            g_running.store(false);
        }
    });
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->cv.wait_for(lock, std::chrono::seconds{3},
                                       [&] { return state->created; }))
            << "LINK_NEVER_CREATED";
    }
    auto client = bus::BusConnection::connect_session();
    aa::core::Result<bus::ParsedMessage> reply{aa::Error{aa::ErrorCode::internal}};
    bool registered = false;
    if (client) {
        const aa::ipc::BusName consumer{"org.custom.AndroidAutoReceiverTest.Consumer"};
        const aa::ipc::BusName service{"org.custom.AndroidAutoReceiver"};
        auto named = client.value().request_name(consumer);
        auto owner = client.value().get_name_owner(service);
        if (named && owner) {
            bus::MessageWriter registration;
            registration.destination(service.value).path("/org/custom/AndroidAutoReceiver")
                .interface_name("org.custom.AndroidAutoReceiver1").member("RegisterConsumer")
                .body_signature("s").put_string(consumer.value);
            auto serial = client.value().send_message(bus::MessageType::method_call, registration);
            if (serial) {
                auto result = client.value().await_reply(serial.value(), owner.value());
                registered = result && result.value().type == bus::MessageType::method_return;
            }
            if (registered) {
                bus::MessageWriter forget;
                forget.destination(service.value).path("/org/custom/AndroidAutoReceiver")
                    .interface_name("org.custom.AndroidAutoReceiver1").member("ForgetPhone")
                    .body_signature("t").put_u64(approved.value().value);
                auto sent = client.value().send_message(bus::MessageType::method_call, forget);
                if (sent) {
                    reply = client.value().await_reply(sent.value(), owner.value());
                }
            }
        }
    }
    if (!registered || !reply) {
        g_running.store(false);
    }
    guard.worker.join();
    watchdog.request_stop();
    watchdog.join();
    const auto output = capture.release();
    faults::set_observer({});
    faults::disarm();
    admission_link_state.reset();
    EXPECT_FALSE(watchdog_fired.load()) << "FORGET_WHILE_GATED_DEADLOCK: " << output;
    EXPECT_TRUE(registered) << output;
    ASSERT_TRUE(reply.has_value()) << output;
    EXPECT_EQ(reply.value().type, bus::MessageType::error_reply);
    bus::Cursor external_error{reply.value().body};
    EXPECT_EQ(external_error.read_string().value(), aa::Error{aa::ErrorCode::helper_unavailable}.message());
    EXPECT_EQ(code, 1) << output;
    EXPECT_EQ(runtime_connects.load(), 1) << output;
    EXPECT_NE(output.find("\"to_state\":\"failed\""), std::string::npos) << output;
    EXPECT_EQ(output.find("\"detail\":\"poll\""), std::string::npos) << output;
    EXPECT_EQ(output.find("\"detail\":\"video_admission\""), std::string::npos) << output;
    EXPECT_EQ(output.find("\"event\":\"active\""), std::string::npos) << output;
    ASSERT_FALSE(mutations.empty()) << "ForgetPhone bypassed the owner queue while gated";
    for (const auto thread : mutations) {
        EXPECT_NE(thread, dispatcher) << "store mutation ran on the D-Bus dispatcher";
    }
    aa::trust::ApprovedPhoneStore persisted{path};
    ASSERT_TRUE(persisted.load().has_value());
    EXPECT_EQ(persisted.lookup(runtime_phone), aa::trust::Decision::unknown);
    std::filesystem::remove_all(root);
}
} // namespace

struct SustainedRun {
    bool opened = false;
    std::size_t writes = 0;
    std::size_t late_after_recovery = 0; // writes > 50 ms late once 2 s past the hold
    std::int64_t phone_lag = 0;
    std::int64_t tail_sink_lag = 0;      // max lateness over the final half
    bool held = false;
};

constexpr std::int64_t kSustainChunkNs = 8192LL * 1'000'000'000LL / 192000;

SustainedRun run_sustained(std::size_t chunks, int hold_ms, std::size_t hold_after_writes) { // NOLINT(bugprone-easily-swappable-parameters): chunk count, hold milliseconds, and write index have distinct units
    CadencedPcmSource source;
    source.chunks = chunks;
    CadencedGstObservation observed;
    runtime_send_audio = true;
    runtime_pcm_source = &source;
    runtime_audio_ack_attempts.store(0);
    runtime_sink_request_override = [&](const aa::audio::detail::SinkOpenRequest &request) {
        auto outcome = real_runtime_sink(request);
        observed.opened = outcome.sink != nullptr;
        if (outcome.sink) {
            outcome.sink = std::make_unique<CadencedRealGstSink>(
                std::move(outcome.sink), observed, hold_ms, hold_after_writes);
        }
        return outcome;
    };
    const auto result = run_runtime(false, 4500 + static_cast<int>(chunks) * 43, true);
    runtime_sink_request_override = {};
    runtime_pcm_source = nullptr;
    runtime_send_audio = false;
    SustainedRun run;
    run.opened = observed.opened;
    if (!run.opened) { return run; }
    EXPECT_EQ(result.code, 0) << result.output;
    EXPECT_EQ(result.output.find("\"event\":\"err\""), std::string::npos) << result.output;
    EXPECT_EQ(source.arrivals.size(), chunks);
    EXPECT_EQ(source.acks.size(), chunks);
    for (std::size_t i = 0; i < source.arrivals.size(); ++i) {
        run.phone_lag = std::max(run.phone_lag,
            source.arrivals[i] - static_cast<std::int64_t>(i) * kSustainChunkNs);
    }
    run.writes = observed.returned_at.size();
    run.held = observed.entered;
    const auto settle = observed.released_at + 2'000'000'000;
    for (std::size_t i = 0; i < run.writes; ++i) {
        const auto lag = observed.returned_at[i] - observed.scheduled_at[i];
        if (i >= run.writes / 2) { run.tail_sink_lag = std::max(run.tail_sink_lag, lag); }
        if ((!run.held || observed.write_at[i] > settle) && lag > 50'000'000) { ++run.late_after_recovery; }
    }
    std::cout << "SUSTAINED hold_ms=" << hold_ms << " writes=" << run.writes << " late_after_recovery="
              << run.late_after_recovery << " phone_lag_us=" << run.phone_lag / 1000
              << " tail_sink_lag_us=" << run.tail_sink_lag / 1000 << '\n';
    return run;
}

// Issue 60: a real-time phone must not be throttled by the receiver's own
// render granularity. PipeWire cannot sustain 5 ms buffers in real time, so a
// sustained cadence (not 8 chunks) is needed to expose cumulative lateness.
TEST(WiredReceiver, RealGstSustainedCadenceKeepsPhoneAndSinkRealTime) {
    const auto run = run_sustained(60, 0, 0);
    if (!run.opened) { GTEST_SKIP() << "real host GStreamer route unavailable"; }
    // A conforming phone is never held back more than one chunk period.
    EXPECT_LE(run.phone_lag, kSustainChunkNs);
    EXPECT_LE(run.tail_sink_lag, 100'000'000);
}

// Issue 60 (Jetson perf-1800 v4): one transient downstream stall left a
// 3-pieces-per-parent stream permanently ~150 ms late, because PipeWire costs
// about one graph cycle per buffer while catching up. After a 100 ms real-sink
// stall the stream must return to real time and stay there.
TEST(WiredReceiver, RealGstMidStreamStallRecoversToRealTime) {
    const auto run = run_sustained(300, 100, 100);
    if (!run.opened) { GTEST_SKIP() << "real host GStreamer route unavailable"; }
    ASSERT_TRUE(run.held);
    EXPECT_EQ(run.late_after_recovery, 0U);
    EXPECT_LE(run.tail_sink_lag, 20'000'000);
}
