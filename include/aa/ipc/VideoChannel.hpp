// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/ipc/VideoSocket.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace aa::ipc {

// Production AF_UNIX SOCK_SEQPACKET video channel (task 22): the task-10 spike
// framing served from /run/user/$UID/android-auto-receiver/video.sock with
// peer-credential admission, a negotiated-limits exchange, bounded queues,
// drop-to-IDR resync and disconnect cleanup. Server = producer (receiver
// media path), client = consumer (decode helper); one consumer at a time.
//
// Ownership: every descriptor and (for the server) its endpoint node are
// released by an idempotent cleanup shared by close() and the Impl
// destructor, so replacing a live channel by move assignment releases what it
// held instead of leaking it. A server unlinks its endpoint only while the
// path still names the exact node it bound (device+inode captured at bind).
//
// Error contract: unauthorized peer -> transport_unauthorized_peer; malformed
// record -> transport_malformed_frame; oversize record or access unit ->
// transport_oversize_frame; handshake shape -> protocol_malformed_message,
// handshake version -> protocol_version_mismatch, out-of-range offer ->
// protocol_negotiation_failed; timeout -> timeout; closed/EOF ->
// transport_closed. Every rejection drives loss (drop-to-IDR) except the
// handshake failures, which close the connection.

struct VideoSocketPath final {
    std::string value{};

    [[nodiscard]] friend bool operator==(const VideoSocketPath&,
                                         const VideoSocketPath&) noexcept = default;
};

[[nodiscard]] VideoSocketPath production_video_socket_path();
[[nodiscard]] VideoSocketPath video_socket_path_in(const std::string& runtime_dir);

// Creates the socket directory (real directory, owned by the caller, 0700)
// and clears the endpoint path only when an existing socket node is verified
// stale (non-blocking datagram probe: connect refused). A live endpoint or a
// non-socket node (regular file, symlink, ...) at the path is refused
// `transport_unavailable` and left untouched; only a stale socket node is
// removed. Malformed path -> invalid_argument; directory failures ->
// transport_unavailable.
[[nodiscard]] core::Result<void> prepare_video_socket_path(const VideoSocketPath& path);

[[nodiscard]] std::uint32_t current_uid() noexcept;

enum class SendStatus : std::uint8_t {
    sent,
    dropped_backpressure,
    dropped_waiting_idr,
};

enum class ReceiveStatus : std::uint8_t {
    fragment,
    complete,
    waiting,
};

class VideoStreamCapture {
public:
    virtual ~VideoStreamCapture() = default;
    VideoStreamCapture() = default;
    VideoStreamCapture(const VideoStreamCapture&) = delete;
    VideoStreamCapture& operator=(const VideoStreamCapture&) = delete;
    VideoStreamCapture(VideoStreamCapture&&) = delete;
    VideoStreamCapture& operator=(VideoStreamCapture&&) = delete;

    [[nodiscard]] virtual core::Result<void> on_access_unit(const VideoAccessUnit& unit) = 0;
};

class VideoProducer final {
public:
    VideoProducer();
    VideoProducer(const VideoProducer&) = delete;
    VideoProducer& operator=(const VideoProducer&) = delete;
    VideoProducer(VideoProducer&&) noexcept;
    VideoProducer& operator=(VideoProducer&&) noexcept;
    ~VideoProducer();

    // Empty payload -> invalid_argument; over the negotiated unit cap ->
    // transport_oversize_frame. drop-to-IDR state suppresses non-IDR units
    // (dropped_waiting_idr) after any backpressure drop.
    [[nodiscard]] core::Result<SendStatus> send_access_unit(const VideoSendRequest& request);

    // shutdown(SHUT_WR) + close; idempotent; the consumer then sees a clean
    // end of stream.
    void close() noexcept;

    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] const NegotiatedLimits& limits() const noexcept;
    [[nodiscard]] std::uint64_t dropped() const noexcept;
    [[nodiscard]] std::uint64_t backpressure_events() const noexcept;
    [[nodiscard]] int native_handle() const noexcept;

    struct Impl;
    explicit VideoProducer(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

class VideoConsumer final {
public:
    VideoConsumer();
    VideoConsumer(const VideoConsumer&) = delete;
    VideoConsumer& operator=(const VideoConsumer&) = delete;
    VideoConsumer(VideoConsumer&&) noexcept;
    VideoConsumer& operator=(VideoConsumer&&) noexcept;
    ~VideoConsumer();

    // timeout.count < 0 blocks, 0 polls. Expired timeout is the typed error
    // `timeout` and mutates nothing. complete() state is stable until the
    // next receive().
    [[nodiscard]] core::Result<ReceiveStatus> receive(core::Milliseconds timeout);

    [[nodiscard]] const VideoAccessUnit& access_unit() const noexcept;

    // Invoked after each reassembled access unit; a capture error is
    // propagated but the stream stays usable.
    void set_capture(VideoStreamCapture* capture) noexcept;

    void close() noexcept;

    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] bool waiting_idr() const noexcept;
    [[nodiscard]] const NegotiatedLimits& limits() const noexcept;
    [[nodiscard]] std::uint64_t resyncs() const noexcept;
    [[nodiscard]] std::uint64_t rejected() const noexcept;
    [[nodiscard]] std::uint64_t suppressed() const noexcept;
    [[nodiscard]] int native_handle() const noexcept;

    struct Impl;
    explicit VideoConsumer(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

struct VideoServerOptions final {
    NegotiatedLimits local_limits{};
    std::uint32_t expected_peer_uid{};
};

struct VideoClientOptions final {
    NegotiatedLimits local_limits{};
    std::uint32_t expected_server_uid{};
    core::Milliseconds timeout{core::Milliseconds{5000}};
};

class VideoServer final {
public:
    VideoServer();
    VideoServer(const VideoServer&) = delete;
    VideoServer& operator=(const VideoServer&) = delete;
    VideoServer(VideoServer&&) noexcept;
    VideoServer& operator=(VideoServer&&) noexcept;
    ~VideoServer();

    // Binds and listens after prepare_video_socket_path; a path held by a
    // live endpoint or a non-socket node is refused transport_unavailable and
    // a failure to secure the new node to 0600 is transport_io with the node
    // removed again (fail closed).
    [[nodiscard]] static core::Result<VideoServer> listen(const VideoSocketPath& path,
                                                          const VideoServerOptions& options);

    // Peer-credential gate then limits exchange. Rejected peers are counted
    // and never returned; the listener stays up. timeout.count < 0 blocks.
    [[nodiscard]] core::Result<VideoProducer> accept_consumer(core::Milliseconds timeout);

    // Closes the listener and unlinks the endpoint node only while it is
    // still the node bound at listen() time; idempotent.
    void close() noexcept;

    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] const VideoSocketPath& path() const noexcept;
    [[nodiscard]] std::uint64_t unauthorized_peers() const noexcept;
    [[nodiscard]] int native_handle() const noexcept;

    struct Impl;
    explicit VideoServer(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

// The client also verifies the server's SO_PEERCRED UID before the limits
// exchange, so a rogue socket at the path is rejected at connect time.
[[nodiscard]] core::Result<VideoConsumer> connect_video_consumer(const VideoSocketPath& path,
                                                                 const VideoClientOptions& options);

} // namespace aa::ipc
