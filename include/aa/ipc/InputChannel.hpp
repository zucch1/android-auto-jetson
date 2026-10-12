// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/ipc/InputSocket.hpp>
#include <aa/ipc/VideoSocket.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace aa::ipc {

// Production AF_UNIX SOCK_SEQPACKET display-input channel (owner ruling
// 2026-10-10): the task-22 socket family served from
// /run/user/$UID/android-auto-receiver/input.sock with the SAME
// peer-credential admission policy (SO_PEERCRED, same-UID only, fail closed).
// The display sends normalized keyboard/D-pad and mouse-to-touch events
// (InputSender); the receiver accepts one sender at a time and reads typed
// events off an InputEndpoint - the seam todo 26's input service consumes.
//
// Ownership: every descriptor and (for the server) its endpoint node are
// released by an idempotent cleanup shared by close() and the Impl
// destructor. A server unlinks its endpoint only while the path still names
// the exact node it bound (device+inode captured at bind).
//
// Error contract: unauthorized peer -> transport_unauthorized_peer (counted,
// listener stays up); malformed record -> transport_malformed_frame; oversize
// record -> transport_oversize_frame; version drift -> protocol_version_mismatch;
// timeout -> timeout; closed/EOF -> transport_closed; congested peer ->
// transport_queue_full. Every record is exactly kInputRecordBytes on the wire,
// so no receive path grows memory.
//
// Sender backpressure (gate review F1): every sender-side socket is
// nonblocking and NO send or connect ever waits - a congested peer cannot
// park the caller (the GUI thread) for even one bounded interval per event,
// so a burst of failed deliveries costs no wall time. Congestion (a full
// accept queue at connect, a full send queue at send) is the typed result
// transport_queue_full and counts as a failed delivery; it is flow control on
// a live peer, NOT a lost peer, so the connection is kept and never
// reconnect-churned. Only a genuine I/O failure on an established fd (the
// peer went away) drops the fd and retries once.

struct InputServerOptions final {
    std::uint32_t expected_peer_uid{};
};

// Display-side sender: forwards normalized events as bounded records. Also an
// InputSink so the Qt surface delivers straight into it (the shipped sink;
// in-memory sinks stay test artifacts). attach() sets a delivery target and
// send()/on_input() then connect lazily, retrying after a failed delivery -
// input is low-rate so at most one connect attempt happens per event, and a
// receiver that appears mid-session is picked up. Delivery never blocks: a
// congested peer yields the typed transport_queue_full and a counted failure
// (dropped()/last_failure()) while the connection stays up. A detached sender
// (no target) drops and counts; every failure is typed and never allocates.
class InputSender final : public InputSink {
public:
    InputSender();
    InputSender(const InputSender&) = delete;
    InputSender& operator=(const InputSender&) = delete;
    InputSender(InputSender&& other) noexcept;
    InputSender& operator=(InputSender&& other) noexcept;
    ~InputSender() override;

    // InputSink: forwards to send(); failures are counted in dropped() and
    // never throw or allocate.
    void on_input(const InputEvent& event) override;

    // Sets the delivery target (receiver endpoint). Idempotent.
    void attach(InputSocketPath path, std::uint32_t expected_server_uid);

    // One record per event. Connects on demand; a detached or still
    // unreachable target yields transport_closed / transport_unavailable.
    [[nodiscard]] core::Result<void> send(const InputEvent& event);

    void close() noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] std::uint64_t sent() const noexcept;
    [[nodiscard]] std::uint64_t dropped() const noexcept;
    // ErrorCode of the most recent failed delivery (nullopt while nothing has
    // failed); dropped() is the delivery-failure count.
    [[nodiscard]] std::optional<ErrorCode> last_failure() const noexcept;
    [[nodiscard]] int native_handle() const noexcept;

    struct Impl;
    explicit InputSender(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

// Receiver-side endpoint (the typed input seam todo 26 consumes): reads one
// bounded record per receive() and decodes it into an InputEvent.
class InputEndpoint final {
public:
    InputEndpoint();
    InputEndpoint(const InputEndpoint&) = delete;
    InputEndpoint& operator=(const InputEndpoint&) = delete;
    InputEndpoint(InputEndpoint&&) noexcept;
    InputEndpoint& operator=(InputEndpoint&&) noexcept;
    ~InputEndpoint();

    // timeout.count < 0 blocks, 0 polls. Expired timeout is the typed error
    // `timeout` and mutates nothing. Malformed/oversize/version-drifted
    // records are rejected typed and counted in rejected(); the endpoint
    // stays usable for the next record.
    [[nodiscard]] core::Result<InputEvent> receive(core::Milliseconds timeout);

    void close() noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] std::uint64_t rejected() const noexcept;
    [[nodiscard]] int native_handle() const noexcept;

    struct Impl;
    explicit InputEndpoint(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

class InputServer final {
public:
    InputServer();
    InputServer(const InputServer&) = delete;
    InputServer& operator=(const InputServer&) = delete;
    InputServer(InputServer&&) noexcept;
    InputServer& operator=(InputServer&&) noexcept;
    ~InputServer();

    // Binds and listens after prepare_input_socket_path; a path held by a
    // live endpoint or a non-socket node is refused transport_unavailable and
    // a failure to secure the new node to 0600 is transport_io with the node
    // removed again (fail closed).
    [[nodiscard]] static core::Result<InputServer> listen(const InputSocketPath& path,
                                                          const InputServerOptions& options);

    // Peer-credential gate. Rejected peers are counted and never returned;
    // the listener stays up. timeout.count < 0 blocks.
    [[nodiscard]] core::Result<InputEndpoint> accept_sender(core::Milliseconds timeout);

    // Closes the listener and unlinks the endpoint node only while it is
    // still the node bound at listen() time; idempotent.
    void close() noexcept;

    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] const InputSocketPath& path() const noexcept;
    [[nodiscard]] std::uint64_t unauthorized_peers() const noexcept;
    [[nodiscard]] int native_handle() const noexcept;

    struct Impl;
    explicit InputServer(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace aa::ipc
