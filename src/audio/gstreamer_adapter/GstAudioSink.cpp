// SPDX-License-Identifier: GPL-3.0-or-later
// Task 25 routing adapter: probe and select the PipeWire-capable sink
// elements (pipewiresink / pulsesink / absent) with the task-23 honesty rule
// and route the mixed stream to the current default output. Adapter zone -
// the only place in the audio library that may include GStreamer headers.
//
// Gate r2 F1/F3: a write is acknowledged only at the RENDERER'S HANDOFF - the
// element's render call actually handing audio to PipeWire/Pulse. The routing
// elements expose that handoff only through their debug traces (GStreamer
// 1.24 offers no render signal on these sinks; the sources show render()
// silently dropping buffers while the stream is not streaming), so a GStreamer
// debug observer counts the handoff events and stream-state transitions per
// element. Element-ingress (pad probe) and chain-return (zero-length marker)
// are kept as separate, explicitly labelled observations and never
// acknowledge a write. Route readiness is confirmed by a render-probe
// round-trip on the CHOSEN route before the sink is returned: the route must
// demonstrably accept and hand off a buffer (a connected, streaming output).
#include "../AudioSinkFactory.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aa::audio {
namespace {

void ensure_gstreamer_initialized() {
    static std::once_flag flag;
    std::call_once(flag, [] { gst_init(nullptr, nullptr); });
}

// Handoff/stream-state observation per routing element (gate r2 F1/F3). The
// routing elements log their PipeWire/Pulse handoff inside the render call
// (`gst_pipewire_sink_render: push buffer`, `gst_pulseringbuffer_commit:
// wrote ... samples`) and their stream-state transitions (`on_state_changed:
// got stream state N`). Those trace lines are the only observable of the real
// handoff, so a debug-observer receives them, keyed by the logging element.
struct HandoffTracker final {
    std::mutex mutex{};
    std::condition_variable cv{};
    std::uint64_t handoff_events{0};
    std::int32_t stream_state{-1};
    // Per-write render window (gate r2 F1, bound at gate r3 F1): opened when
    // the write's nonempty data buffer enters the element, closed when its
    // marker arrives. A handoff event acknowledges ONLY a window it can be
    // bound to: same rendering/stream thread as the window, emitting element
    // matching this route's sink (or no element object), and a window seq
    // above the last credited one. Unmatched or ambiguous events fail closed
    // - never credited to whichever window happens to be open.
    bool window_open{false};
    std::uint64_t window_seq{0};
    std::uint64_t acked_seq{0};
    std::thread::id window_thread{};
};

std::mutex g_trackers_mutex{};
std::map<const GstObject*, HandoffTracker*> g_trackers{};
// Process-wide observability (gate r3 F1): every handoff-signature trace line
// seen in this process, credited or not. Lets tests prove foreign pipelines'
// traffic is observed and rejected rather than silently absent.
std::atomic<std::uint64_t> g_handoff_events_seen{0};

void register_tracker(GstObject* element, HandoffTracker* tracker) {
    std::lock_guard<std::mutex> lock(g_trackers_mutex);
    g_trackers[element] = tracker;
}

void unregister_tracker(GstObject* element) {
    std::lock_guard<std::mutex> lock(g_trackers_mutex);
    g_trackers.erase(element);
}

void handoff_log_observer(GstDebugCategory*, GstDebugLevel, const gchar*, const gchar* function,
                          gint, GObject* object, GstDebugMessage* message, gpointer) {
    if (function == nullptr) {
        return;
    }
    const gchar* text = message != nullptr ? gst_debug_message_get(message) : nullptr;
    if (text == nullptr) {
        return;
    }
    const std::string_view line{text};

    const bool is_handoff = (std::strcmp(function, "gst_pipewire_sink_render") == 0
                            && line.find("push buffer") != std::string_view::npos)
        || (std::strcmp(function, "gst_pulseringbuffer_commit") == 0
            && line.find("wrote") != std::string_view::npos
            && line.find("wrote 0") == std::string_view::npos);

    if (is_handoff) {
        g_handoff_events_seen.fetch_add(1, std::memory_order_relaxed);
        // Gate r3 F1: bind the event to the write that produced it. The
        // callback runs synchronously on the emitting (rendering/stream)
        // thread and carries the emitting element object when the element
        // logs one; an event is credited only when it matches ONE open
        // window's thread AND that route's sink element, with a window seq
        // above the last credited. A foreign pipeline's pushes render on
        // their own streaming thread and can never match; anything
        // unmatchable fails closed (no credit -> the write times out typed).
        const std::thread::id event_thread = std::this_thread::get_id();
        const GstObject* event_object = object != nullptr ? GST_OBJECT(object) : nullptr;
        HandoffTracker* single = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_trackers_mutex);
            for (const auto& [element, tracker] : g_trackers) {
                std::lock_guard<std::mutex> tracker_lock(tracker->mutex);
                const bool thread_match = tracker->window_open
                    && tracker->window_thread == event_thread;
                // The registered map key IS this route's sink element (gate
                // r4 F1) - compare against it directly; a null tracker field
                // must never reject legitimate handoffs.
                const bool object_match = event_object == nullptr || event_object == element;
                const bool seq_match = tracker->window_seq > tracker->acked_seq;
                if (thread_match && object_match && seq_match) {
                    if (single != nullptr) {
                        // Ambiguous: two windows could claim it - fail closed.
                        single = nullptr;
                        break;
                    }
                    single = tracker;
                }
            }
        }
        if (single == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(single->mutex);
        if (!single->window_open || single->window_thread != event_thread
            || single->window_seq <= single->acked_seq) {
            return;
        }
        single->acked_seq = single->window_seq;
        single->window_open = false;
        single->handoff_events += 1;
        single->cv.notify_all();
        return;
    }

    // Non-handoff lines (stream-state transitions) are observability only.
    // The trace lines are object-less; attribute to the single registered
    // route and drop otherwise - never used for acknowledgements.
    HandoffTracker* tracker = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_trackers_mutex);
        if (object != nullptr) {
            const auto it = g_trackers.find(GST_OBJECT(object));
            if (it != g_trackers.end()) {
                tracker = it->second;
            }
        } else if (g_trackers.size() == 1) {
            tracker = g_trackers.begin()->second;
        }
    }
    if (tracker == nullptr) {
        return;
    }
    if (std::strcmp(function, "on_state_changed") == 0
        && line.find("got stream state") != std::string_view::npos) {
        const std::size_t digits = line.find_last_of("0123456789");
        if (digits != std::string_view::npos) {
            const std::size_t begin = line.find_last_not_of("0123456789", digits);
            const std::string number{line.substr(begin == std::string_view::npos ? 0 : begin + 1)};
            std::lock_guard<std::mutex> lock(tracker->mutex);
            tracker->stream_state = static_cast<std::int32_t>(std::strtol(number.c_str(), nullptr, 10));
            tracker->cv.notify_all();
        }
    }
}

// Keeps GStreamer trace output working exactly as the user configured it
// (GST_DEBUG=...) while letting the handoff observer run at the thresholds it
// needs without spamming stderr during normal test runs.
void quiet_default_log_handler(GstDebugCategory* category, GstDebugLevel level, const gchar* file,
                               const gchar* function, gint line, GObject* object,
                               GstDebugMessage* message, gpointer) {
    if (g_getenv("GST_DEBUG") != nullptr) {
        gst_debug_log_default(category, level, file, function, line, object, message, nullptr);
    }
}

void install_handoff_observer() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        gst_debug_remove_log_function(gst_debug_log_default);
        gst_debug_add_log_function(quiet_default_log_handler, nullptr, nullptr);
        gst_debug_add_log_function(handoff_log_observer, nullptr, nullptr);
    });
}

// The handoff lines are TRACE-level in the elements' sources
// (gst_pipewire_sink_render logs "push buffer" at GST_TRACE), and a category
// only registers when its element class initializes - so the thresholds are
// applied AFTER the elements have been probed; before that the names do not
// exist and the set is a no-op.
void enable_handoff_thresholds() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        gst_debug_set_threshold_for_name("pipewiresink", GST_LEVEL_TRACE);
        gst_debug_set_threshold_for_name("pulse", GST_LEVEL_TRACE);
    });
}

struct KnownSink final {
    const char* element;
    bool pipewire_capable;
};

// Routing elements are probed, never assumed. pipewiresink routes to PipeWire
// natively; pulsesink routes via the PulseAudio-compatible layer (pipewire-pulse
// on a PipeWire host) and is the recorded fallback.
constexpr KnownSink kKnownSinks[] = {
    {"pipewiresink", true},
    {"pulsesink", true},
};

[[nodiscard]] SinkCandidate probe_candidate(const KnownSink& known) {
    SinkCandidate candidate;
    candidate.element = known.element;
    candidate.pipewire_capable = known.pipewire_capable;

    GstElementFactory* factory = gst_element_factory_find(known.element);
    if (factory == nullptr) {
        candidate.available = false;
        candidate.viable = false;
        candidate.cause = "Element not found in GStreamer registry";
        return candidate;
    }
    gst_object_unref(factory);
    candidate.available = true;

    GstElement* element = gst_element_factory_make(known.element, "aa_audio_sink_probe");
    if (element == nullptr) {
        candidate.viable = false;
        candidate.cause = "Failed to instantiate element";
        return candidate;
    }
    const GstStateChangeReturn state = gst_element_set_state(element, GST_STATE_READY);
    if (state == GST_STATE_CHANGE_FAILURE) {
        candidate.viable = false;
        candidate.cause = "Failed to transition to GST_STATE_READY";
        gst_element_set_state(element, GST_STATE_NULL);
        gst_object_unref(element);
        return candidate;
    }
    gst_element_set_state(element, GST_STATE_NULL);
    gst_object_unref(element);
    candidate.viable = true;
    candidate.cause = "Instantiated and transitioned to READY successfully";
    return candidate;
}

// Element-ingress statistics at the routing element's sink pad (data buffers
// only) plus the chain-return observation (zero-length marker reaching the pad
// after the data buffer's chain returned). The marker is DROPPED at this pad:
// it exists only to prove ordering and must never render, so marker traffic
// cannot produce a handoff event. The probe also maintains the per-write
// render window used to correlate a handoff event with its SPECIFIC nonempty
// write (gate r2 F1).
class PadProbe final {
public:
    PadProbe(HandoffTracker* tracker, bool drop_nonempty, bool drop_after_window) noexcept
        : tracker_(tracker), drop_nonempty_(drop_nonempty),
          drop_after_window_(drop_after_window) {}

    // The fault is armed only after route readiness (the GDB scenario injects
    // it mid-session, not during the startup render probe).
    void arm_drop_nonempty() noexcept { drop_nonempty_ = true; }
    void arm_drop_after_window() noexcept { drop_after_window_ = true; }
    [[nodiscard]] GstPadProbeReturn on_buffer(GstBuffer* buffer) {
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            return GST_PAD_PROBE_OK;
        }
        const std::size_t values = map.size / sizeof(std::int16_t);
        if (map.size == 0) {
            gst_buffer_unmap(buffer, &map);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                chain_return_buffers_ += 1;
                chain_return_samples_ += pending_chain_samples_;
                pending_chain_samples_ = 0;
                cv_.notify_all();
            }
            return GST_PAD_PROBE_DROP;
        }

        std::int32_t local_min = std::numeric_limits<std::int32_t>::max();
        std::int32_t local_max = std::numeric_limits<std::int32_t>::min();
        for (std::size_t i = 0; i < values; ++i) {
            std::int16_t sample{};
            std::memcpy(&sample, map.data + i * sizeof(std::int16_t), sizeof(sample));
            local_min = local_min < sample ? local_min : sample;
            local_max = local_max > sample ? local_max : sample;
        }
        gst_buffer_unmap(buffer, &map);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            buffers_ += 1;
            samples_ += values;
            pending_chain_samples_ += values;
            if (values > 0) {
                if (!seen_ || local_min < min_sample_) {
                    min_sample_ = local_min;
                }
                if (!seen_ || local_max > max_sample_) {
                    max_sample_ = local_max;
                }
                seen_ = true;
            }
        }
        cv_.notify_all();
        if (drop_nonempty_) {
            // Test-only fault injection (gate r2 F1 regression): the write's
            // nonempty buffer is dropped right here, so it can never render
            // and no handoff window opens. Never an ack path.
            return GST_PAD_PROBE_DROP;
        }
        std::uint64_t write_identity = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            write_identity = expected_writes_;
        }
        {
            std::lock_guard<std::mutex> tracker_lock(tracker_->mutex);
            tracker_->window_seq = write_identity;
            tracker_->window_open = true;
            tracker_->window_thread = std::this_thread::get_id();
        }
        if (drop_after_window_) {
            // Gate r3 F1 GDB scenario: the window is open (a foreign
            // pipeline's pushes are trying to claim it) but this write's
            // buffer never renders. Only a bound handoff may acknowledge it.
            return GST_PAD_PROBE_DROP;
        }
        return GST_PAD_PROBE_OK;
    }

    [[nodiscard]] std::uint64_t expect_write() {
        std::lock_guard<std::mutex> lock(mutex_);
        expected_writes_ += 1;
        return expected_writes_;
    }

    // Waits for this write's zero-length marker at the pad on the SHARED
    // write deadline (gate fix 3): chain settlement, enqueue admission and the
    // render handoff all consume one absolute budget, never stacked windows.
    [[nodiscard]] bool wait_chain_return_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_until(lock, deadline,
                              [this] { return chain_return_buffers_ >= expected_writes_; });
    }

    void count_drain_completed() {
        std::lock_guard<std::mutex> lock(mutex_);
        drains_completed_ += 1;
    }

    void reset_observations() {
        std::lock_guard<std::mutex> lock(mutex_);
        buffers_ = 0;
        samples_ = 0;
        pending_chain_samples_ = 0;
        chain_return_buffers_ = 0;
        chain_return_samples_ = 0;
        expected_writes_ = 0;
        min_sample_ = 0;
        max_sample_ = 0;
        seen_ = false;
    }

    [[nodiscard]] SinkProbeStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        SinkProbeStats out;
        out.buffers = buffers_;
        out.samples = samples_;
        out.min_sample = seen_ ? min_sample_ : 0;
        out.max_sample = seen_ ? max_sample_ : 0;
        return out;
    }

    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> chain_return() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {chain_return_buffers_, chain_return_samples_};
    }

    [[nodiscard]] std::uint64_t drains_completed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return drains_completed_;
    }

private:
    HandoffTracker* tracker_{nullptr};
    bool drop_nonempty_{false};
    bool drop_after_window_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t buffers_{0};
    std::uint64_t samples_{0};
    std::uint64_t pending_chain_samples_{0};
    std::uint64_t chain_return_buffers_{0};
    std::uint64_t chain_return_samples_{0};
    std::uint64_t expected_writes_{0};
    std::uint64_t drains_completed_{0};
    std::int32_t min_sample_{0};
    std::int32_t max_sample_{0};
    bool seen_{false};
};

[[nodiscard]] GstPadProbeReturn pad_probe_trampoline(GstPad*, GstPadProbeInfo* info, gpointer user) {
    if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer == nullptr) {
        return GST_PAD_PROBE_OK;
    }
    return static_cast<PadProbe*>(user)->on_buffer(buffer);
}

// Bounded appsrc admission (gate fix 3). The appsrc is configured with these
// queue limits, and the APPLICATION enforces them on its own pushes: with
// block=FALSE the queue limits are thresholds only (appsrc keeps accepting
// buffers past them, see gst_app_src_push_internal), so write admission must
// stop pushing while the queue cannot hold the COMPLETE data+marker pair.
constexpr std::uint64_t kAppsrcMaxBytes = 65536;
constexpr std::uint64_t kAppsrcMaxBuffers = 2;
// One write enqueues exactly one pair: the nonempty data buffer plus its
// zero-length chain-return marker. The marker occupies one buffer slot.
constexpr std::uint64_t kWriteBufferPair = 2;
// 64 KiB whole-frame bound for one private write (the router splits larger
// intervals to the same bound); an oversized private write is rejected typed.
constexpr std::uint64_t kMaxWriteBytes = 65536;
// Admission polls the queue levels and the bus with naps bounded by this;
// a nap is never longer than the remaining write budget (gate fix 3).
constexpr std::chrono::microseconds kAdmissionPollNap{500};

// Per-write outcome (gate fix 3): the typed result plus whether this write's
// marker reached the element (fully consumed). Startup readiness probes are
// retried ONLY when the previous probe was consumed without a handoff.
struct WriteOutcome final {
    core::Result<void> result{};
    bool marker_settled{false};
};

struct SinkElements final {
    GstElement* pipeline{nullptr};
    GstElement* appsrc{nullptr};
};

class GstSink final : public detail::ObservedAudioSink {
public:
    GstSink(SinkElements elements, AudioFormat format, core::Milliseconds write_timeout,
            GstElement* sink_element, bool drop_nonempty, bool drop_after_window)
        : pipeline_(elements.pipeline), appsrc_(elements.appsrc), sink_element_(sink_element),
          format_(format), write_timeout_(write_timeout),
          probe_(&handoff_, false, false) {
        (void)drop_nonempty;
        (void)drop_after_window;
        pending_drop_nonempty_ = drop_nonempty;
        pending_drop_after_window_ = drop_after_window;
        register_tracker(GST_OBJECT(sink_element_), &handoff_);
    }

    ~GstSink() override { release(); }

    [[nodiscard]] bool attach_probe() {
        pad_ = gst_element_get_static_pad(sink_element_, "sink");
        if (pad_ == nullptr) {
            return false;
        }
        probe_id_ = gst_pad_add_probe(pad_, GST_PAD_PROBE_TYPE_BUFFER, pad_probe_trampoline,
                                      &probe_, nullptr);
        return probe_id_ != 0;
    }

    [[nodiscard]] core::Result<void> write(const MixedChunk& chunk) override {
        WriteOutcome outcome = write_with_timeout(chunk, write_timeout_, false);
        return outcome.result;
    }

    // Bounded nonblocking enqueue (gate fix 3). ONE absolute deadline is
    // computed at entry and shared by queue admission, chain settlement and
    // the render handoff - never two stacked timeout windows. Capacity for the
    // COMPLETE data+marker pair is reserved BEFORE anything is allocated or
    // pushed; the pair is pushed nonblocking; a buffer handed to
    // gst_app_src_push_buffer() is NEVER retried (the push takes ownership
    // even when it fails). A NORMAL write that times out or loses the route
    // latches this sink failed - later normal writes fail immediately, so a
    // late render callback can never resurrect a failed write as success.
    // Readiness probes never latch; confirm_route's retry rule governs them.
    [[nodiscard]] WriteOutcome write_with_timeout(const MixedChunk& chunk,
                                                  core::Milliseconds timeout,
                                                  bool readiness_probe) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout.count);
        WriteOutcome outcome;
        if (pipeline_ == nullptr || released_) {
            return fail_write(outcome, Error{ErrorCode::audio_sink_unavailable},
                              readiness_probe);
        }
        if (!readiness_probe && failed_write_latched_) {
            outcome.result = Error{ErrorCode::audio_sink_unavailable};
            return outcome;
        }
        if (!(chunk.format == format_) || chunk.pcm.empty()) {
            outcome.result = Error{ErrorCode::audio_format_unsupported};
            return outcome;
        }
        const std::uint64_t frame_bytes =
            static_cast<std::uint64_t>(format_.channels) * sizeof(std::int16_t);
        if (frame_bytes == 0 || chunk.pcm.size() % frame_bytes != 0) {
            outcome.result = Error{ErrorCode::audio_format_unsupported};
            return outcome;
        }
        // The 64 KiB whole-frame bound for one private write (the router
        // splits larger intervals to the same bound). An oversized private
        // write is a format failure, never a partial enqueue.
        const std::uint64_t data_bytes = chunk.pcm.size();
        if (data_bytes > kMaxWriteBytes) {
            outcome.result = Error{ErrorCode::audio_format_unsupported};
            return outcome;
        }

        const auto bus_error = poll_bus_error();
        if (bus_error.has_value()) {
            return fail_write(outcome, *bus_error, readiness_probe);
        }

        // Explicit queue admission (gate fix 3). block=FALSE means the appsrc
        // limits are thresholds only (its push keeps accepting past them), so
        // the APPLICATION stops pushing while the queue cannot hold the
        // COMPLETE data+marker pair: queued bytes + this write's bytes fit the
        // byte limit and the queued buffers + the pair fit the buffer limit.
        // While there is no capacity the levels and the bus are polled with
        // naps bounded by the remaining budget - never past the deadline.
        for (;;) {
            const std::pair<std::uint64_t, std::uint64_t> levels = appsrc_queue_levels();
            if (levels.first + data_bytes <= kAppsrcMaxBytes
                && levels.second + kWriteBufferPair <= kAppsrcMaxBuffers) {
                break;
            }
            const auto full_bus_error = poll_bus_error();
            if (full_bus_error.has_value()) {
                return fail_write(outcome, *full_bus_error, readiness_probe);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return fail_write(outcome, Error{ErrorCode::timeout}, readiness_probe);
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
            std::this_thread::sleep_for(remaining < kAdmissionPollNap ? remaining
                                                                      : kAdmissionPollNap);
        }

        // Allocation and copy happen only AFTER admission (the queued bytes
        // and slots are the scarce resource) and the deadline is rechecked so
        // a slow path can never push past the budget.
        if (std::chrono::steady_clock::now() >= deadline) {
            return fail_write(outcome, Error{ErrorCode::timeout}, readiness_probe);
        }
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, chunk.pcm.size(), nullptr);
        if (buffer == nullptr) {
            return fail_write(outcome, Error{ErrorCode::audio_sink_unavailable},
                              readiness_probe);
        }
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            gst_buffer_unref(buffer);
            return fail_write(outcome, Error{ErrorCode::audio_sink_unavailable},
                              readiness_probe);
        }
        std::memcpy(map.data, chunk.pcm.data(), chunk.pcm.size());
        gst_buffer_unmap(buffer, &map);
        GST_BUFFER_PTS(buffer) = chunk_pts(chunk.timestamp);

        // Chain-return observation: the zero-length marker behind the data
        // buffer reaches the sink pad only after the data buffer's chain
        // returned. Tracked, but it does NOT acknowledge the write.
        GstBuffer* marker = gst_buffer_new_allocate(nullptr, 0, nullptr);
        if (marker == nullptr) {
            gst_buffer_unref(buffer);
            return fail_write(outcome, Error{ErrorCode::audio_sink_unavailable},
                              readiness_probe);
        }
        GST_BUFFER_PTS(marker) = GST_BUFFER_PTS(buffer);

        const std::uint64_t write_seq = probe_.expect_write();
        GstFlowReturn pushed = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
        if (pushed != GST_FLOW_OK) {
            // The failed push already consumed the data buffer (ownership
            // transfers even on failure); the marker was NEVER pushed and
            // must be released here or it leaks.
            gst_buffer_unref(marker);
            return fail_write(outcome, Error{ErrorCode::audio_sink_unavailable},
                              readiness_probe);
        }
        pushed = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), marker);
        if (pushed != GST_FLOW_OK) {
            // Same ownership rule on the marker; nothing is ever retried
            // after gst_app_src_push_buffer() has taken a buffer.
            return fail_write(outcome, Error{ErrorCode::audio_sink_unavailable},
                              readiness_probe);
        }

        // The write is acknowledged ONLY at the renderer's handoff (gate r2
        // F1): the element's render call actually handed the audio to
        // PipeWire/Pulse. A silently dropped buffer (stream not streaming)
        // never produces a handoff and times out here instead of being
        // acknowledged.
        // Both observations must be settled before the write returns:
        // the marker (chain-return) is pushed after the data buffer, so the
        // handoff alone can fire while the marker is still in flight and a
        // stats snapshot would race it. Both waits run against the ONE
        // deadline computed at entry (gate fix 3): admission, chain
        // settlement and the handoff share a single budget.
        const bool chained = probe_.wait_chain_return_until(deadline);
        const bool handed = wait_handoff_until(write_seq, deadline);
        close_window();
        outcome.marker_settled = chained;
        if (!handed || !chained) {
            const auto late_error = poll_bus_error();
            if (late_error.has_value()) {
                return fail_write(outcome, *late_error, readiness_probe);
            }
            return fail_write(outcome, Error{ErrorCode::timeout}, readiness_probe);
        }
        return outcome;
    }

    // Render-probe round-trip (gate r2 F3): the CHOSEN route must
    // demonstrably accept and hand off a buffer before the sink is returned.
    // Probes are retried within the bound because the element silently drops
    // buffers until its stream is streaming (link setup latency). A readiness
    // probe is retried ONLY when it was fully consumed without a handoff (its
    // marker settled and the pipeline dropped it); if the marker never
    // settled the probe is still unresolved and readiness probing stops -
    // no piling new attempts behind unresolved work (gate fix 3).
    [[nodiscard]] bool confirm_route(core::Milliseconds bound) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(bound.count);
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }
            if (poll_bus_error().has_value()) {
                return false;
            }
            std::vector<std::byte> silence(
                static_cast<std::size_t>(format_.channels) * sizeof(std::int16_t), std::byte{0});
            MixedChunk probe_chunk;
            probe_chunk.format = format_;
            // Same monotonic clock the router stamps chunks with: this probe
            // anchors the stream-relative PTS origin, and a zero origin here
            // would push every later chunk into the far future under sync=TRUE.
            probe_chunk.timestamp = core::Nanoseconds{
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()};
            probe_chunk.pcm = std::span<const std::byte>{silence.data(), silence.size()};
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            const auto slice = remaining.count() < 300 ? remaining.count() : 300;
            const WriteOutcome attempt =
                write_with_timeout(probe_chunk, core::Milliseconds{slice > 0 ? slice : 1}, true);
            if (attempt.result.has_value()) {
                return true;
            }
            if (!attempt.marker_settled) {
                return false;
            }
        }
    }

    [[nodiscard]] core::Result<void> drain(core::Milliseconds timeout) override {
        if (pipeline_ == nullptr || released_) {
            return Error{ErrorCode::audio_sink_unavailable};
        }
        if (drained_) {
            return {};
        }
        const auto bus_error = poll_bus_error();
        if (bus_error.has_value()) {
            return *bus_error;
        }

        // End-of-stream round-trip: the element's render path finishes the
        // queued work before teardown. This is a bookkeeping barrier only -
        // it makes no device-consumption or delivery guarantee (gate r2 F1).
        const GstFlowReturn eos = gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
        if (eos != GST_FLOW_OK) {
            return Error{ErrorCode::audio_sink_unavailable};
        }
        GstBus* bus = gst_element_get_bus(pipeline_);
        GstMessage* message = bus != nullptr
            ? gst_bus_timed_pop_filtered(bus, static_cast<GstClockTime>(timeout.count) * 1000ULL * 1000ULL,
                                         GST_MESSAGE_EOS)
            : nullptr;
        if (bus != nullptr) {
            gst_object_unref(bus);
        }
        if (message == nullptr) {
            const auto late_error = poll_bus_error();
            if (late_error.has_value()) {
                return *late_error;
            }
            return Error{ErrorCode::timeout};
        }
        gst_message_unref(message);
        drained_ = true;
        probe_.count_drain_completed();
        return {};
    }

    [[nodiscard]] SinkProbeStats probe_stats() const override { return probe_.stats(); }

    [[nodiscard]] RenderBarrierStats render_stats() const override {
        RenderBarrierStats out;
        const auto chain = probe_.chain_return();
        out.chain_return_buffers = chain.first;
        out.chain_return_samples = chain.second;
        out.drains_completed = probe_.drains_completed();
        std::lock_guard<std::mutex> lock(handoff_.mutex);
        out.handoff_events = handoff_.handoff_events;
        out.stream_state = handoff_.stream_state;
        out.handoff_events_seen = g_handoff_events_seen.load(std::memory_order_relaxed);
        return out;
    }

    void arm_fault_injection() noexcept {
        if (pending_drop_nonempty_) {
            probe_.arm_drop_nonempty();
        }
        if (pending_drop_after_window_) {
            probe_.arm_drop_after_window();
        }
    }

    void reset_observations() {
        probe_.reset_observations();
        std::lock_guard<std::mutex> lock(handoff_.mutex);
        handoff_.handoff_events = 0;
        handoff_.acked_seq = 0;
        handoff_.window_seq = 0;
        handoff_.window_open = false;
    }

    // End-of-stream round-trip BEFORE NULL (bounded and best-effort): the
    // element's render path gets to finish queued work before teardown. No
    // delivery or device-consumption claim is attached to this.
    void release() noexcept {
        // Teardown must never stall on a test-only downstream block (gate fix
        // 3): release it first, the tests also release it on every exit path.
        disarm_downstream_block();
        if (pipeline_ != nullptr && !released_ && !drained_) {
            (void)drain(core::Milliseconds{2000});
        }
        released_ = true;
        unregister_tracker(GST_OBJECT(sink_element_));
        if (pipeline_ != nullptr) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }
        if (pad_ != nullptr && probe_id_ != 0) {
            gst_pad_remove_probe(pad_, probe_id_);
        }
        if (pad_ != nullptr) {
            gst_object_unref(pad_);
            pad_ = nullptr;
        }
        if (pipeline_ != nullptr) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
        appsrc_ = nullptr;
        sink_element_ = nullptr;
        probe_id_ = 0;
    }

private:
    // Stream-relative PTS: the routing element clock-syncs (sync=TRUE) and
    // the receiver timestamps are epoch-scale A/V clock values the pipeline
    // clock cannot schedule against. The first write anchors the origin.
    [[nodiscard]] GstClockTime chunk_pts(core::Nanoseconds timestamp) {
        if (!pts_origin_set_) {
            pts_origin_set_ = true;
            pts_origin_ns_ = timestamp.count;
        }
        const std::int64_t delta = timestamp.count - pts_origin_ns_;
        return static_cast<GstClockTime>(delta > 0 ? delta : 0);
    }

    void close_window() {
        std::lock_guard<std::mutex> lock(handoff_.mutex);
        handoff_.window_open = false;
    }

    // Waits for the handoff credit of the SPECIFIC write identified by seq
    // (gate r3 F1): only an event bound to that write's window can advance
    // acked_seq to it. Runs against the write's ONE deadline (gate fix 3) so
    // chain settlement and this handoff wait share a single budget.
    [[nodiscard]] bool wait_handoff_until(std::uint64_t seq,
                                          std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(handoff_.mutex);
        return handoff_.cv.wait_until(lock, deadline, [&] { return handoff_.acked_seq >= seq; });
    }

    // Terminal-write bookkeeping (gate fix 3): a NORMAL write failure latches
    // the sink so every later write fails immediately (a late render callback
    // can never resurrect it); readiness probes never latch.
    [[nodiscard]] WriteOutcome fail_write(WriteOutcome outcome, Error error, bool readiness_probe) {
        if (!readiness_probe) {
            failed_write_latched_ = true;
        }
        outcome.result = error;
        return outcome;
    }

    // Queue levels of the appsrc (bytes, buffers): the admission observables.
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> appsrc_queue_levels() const {
        std::uint64_t bytes = 0;
        std::uint64_t buffers = 0;
        if (appsrc_ != nullptr) {
            bytes = gst_app_src_get_current_level_bytes(GST_APP_SRC(appsrc_));
            buffers = gst_app_src_get_current_level_buffers(GST_APP_SRC(appsrc_));
        }
        return {bytes, buffers};
    }

public:
    // ---- Test-only downstream block + appsrc observation (gate fix 3) ----
    // Driven through the private hooks in AudioSinkFactory.hpp; never used by
    // production callers. The block is a blocking probe on the routing
    // element's sink pad: GST_PAD_PROBE_OK from a blocking probe keeps the pad
    // blocked, so upstream buffers stall in front of the element and the
    // appsrc queue fills behind them.

    [[nodiscard]] bool arm_downstream_block() {
        if (pad_ == nullptr || released_) {
            return false;
        }
        if (block_probe_id_ != 0) {
            return true;
        }
        block_entered_ = false;
        block_probe_id_ = gst_pad_add_probe(pad_, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                                            block_probe_trampoline, this, nullptr);
        return block_probe_id_ != 0;
    }

    // True once the block has actually HELD a buffer - the stall is engaged
    // and later pushes can only accumulate in the appsrc queue.
    [[nodiscard]] bool wait_downstream_block_entered(core::Milliseconds timeout) {
        std::unique_lock<std::mutex> lock(block_mutex_);
        return block_cv_.wait_for(lock, std::chrono::milliseconds(timeout.count),
                                  [this] { return block_entered_; });
    }

    void disarm_downstream_block() noexcept {
        if (pad_ != nullptr && block_probe_id_ != 0) {
            gst_pad_remove_probe(pad_, block_probe_id_);
        }
        block_probe_id_ = 0;
        std::lock_guard<std::mutex> lock(block_mutex_);
        block_entered_ = false;
    }

    // Pushes raw zero-filled buffers straight into appsrc (no render barrier,
    // no handoff acknowledgement) until the queued byte level reaches
    // target_bytes. Returns the level reached.
    [[nodiscard]] std::uint64_t prefill_appsrc(std::uint64_t target_bytes) {
        if (appsrc_ == nullptr || released_) {
            return 0;
        }
        const std::uint64_t chunk_bytes = std::max<std::uint64_t>(
            1, std::min<std::uint64_t>(target_bytes, kMaxWriteBytes / 2));
        // One buffer may be held outside the queue (in the blocked chain), so
        // the push budget is a pair of chunks above the target; never unbounded.
        const std::uint64_t push_budget = target_bytes + 2 * chunk_bytes;
        std::uint64_t pushed = 0;
        while (appsrc_queue_levels().first < target_bytes && pushed < push_budget) {
            GstBuffer* buffer =
                gst_buffer_new_allocate(nullptr, static_cast<gsize>(chunk_bytes), nullptr);
            if (buffer == nullptr) {
                break;
            }
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
                std::memset(map.data, 0, map.size);
                gst_buffer_unmap(buffer, &map);
            }
            if (gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer) != GST_FLOW_OK) {
                break;
            }
            pushed += chunk_bytes;
        }
        return appsrc_queue_levels().first;
    }

    [[nodiscard]] detail::AppsrcQueueLevel appsrc_queue_level() const {
        detail::AppsrcQueueLevel level;
        const std::pair<std::uint64_t, std::uint64_t> levels = appsrc_queue_levels();
        level.queued_bytes = levels.first;
        level.queued_buffers = levels.second;
        if (appsrc_ != nullptr) {
            level.max_bytes = gst_app_src_get_max_bytes(GST_APP_SRC(appsrc_));
            level.max_buffers = gst_app_src_get_max_buffers(GST_APP_SRC(appsrc_));
        }
        return level;
    }

    static GstPadProbeReturn block_probe_trampoline(GstPad*, GstPadProbeInfo* info, gpointer user) {
        return static_cast<GstSink*>(user)->on_downstream_block(info);
    }

    [[nodiscard]] GstPadProbeReturn on_downstream_block(GstPadProbeInfo* info) {
        if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
            {
                std::lock_guard<std::mutex> lock(block_mutex_);
                block_entered_ = true;
            }
            block_cv_.notify_all();
        }
        // Blocking probes block on GST_PAD_PROBE_OK by default: the data item
        // stays held here until the probe is removed.
        return GST_PAD_PROBE_OK;
    }

private:
    // Asynchronous route failures (bus errors) are observed at every write and
    // drain so a mid-stream output loss surfaces typed instead of silent.
    [[nodiscard]] std::optional<Error> poll_bus_error() const {
        if (pipeline_ == nullptr) {
            return Error{ErrorCode::audio_sink_unavailable};
        }
        GstBus* bus = gst_element_get_bus(pipeline_);
        if (bus == nullptr) {
            return Error{ErrorCode::audio_sink_unavailable};
        }
        GstMessage* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        if (message == nullptr) {
            return std::nullopt;
        }
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        if (error != nullptr) {
            g_error_free(error);
        }
        g_free(debug);
        gst_message_unref(message);
        return Error{ErrorCode::audio_sink_unavailable};
    }

    GstElement* pipeline_{nullptr};
    GstElement* appsrc_{nullptr};
    GstElement* sink_element_{nullptr};
    GstPad* pad_{nullptr};
    gulong probe_id_{0};
    AudioFormat format_{};
    core::Milliseconds write_timeout_{2000};
    mutable HandoffTracker handoff_{};
    PadProbe probe_;
    bool pending_drop_nonempty_{false};
    bool pending_drop_after_window_{false};
    bool pts_origin_set_{false};
    std::int64_t pts_origin_ns_{0};
    bool drained_{false};
    bool released_{false};
    // Failed-write latch (gate fix 3): set by a normal write that timed out or
    // lost the route; every later normal write fails immediately.
    bool failed_write_latched_{false};
    // Test-only downstream block state (gate fix 3), see arm_downstream_block.
    gulong block_probe_id_{0};
    std::mutex block_mutex_{};
    std::condition_variable block_cv_{};
    bool block_entered_{false};
};

// Test-only (gate r3 F1): unrelated in-process traffic source. Its render
// pushes log handoff-signature lines on their own streaming thread - the
// attribution binding must reject them.
GstElement* g_foreign_pipeline = nullptr;

void release_elements(std::initializer_list<GstElement*> elements) {
    for (GstElement* element : elements) {
        if (element != nullptr) {
            gst_object_unref(element);
        }
    }
}

// Explicit-target policy bound to the selected backend (gate r2 F3): a
// pipewiresink target must resolve to a PipeWire output (the provider's own
// keys), so generic Audio/Sink enumeration from OTHER providers can never
// satisfy a route it does not belong to.
[[nodiscard]] bool device_matches_target(GstDevice* device, const std::string& target) {
    if (target.empty()) {
        return true;
    }
    GstStructure* props = gst_device_get_properties(device);
    if (props == nullptr) {
        return false;
    }
    bool hit = false;
    for (const char* key : {"node.name", "object.serial"}) {
        const gchar* value = gst_structure_get_string(props, key);
        if (value != nullptr && target == value) {
            hit = true;
            break;
        }
    }
    gst_structure_free(props);
    return hit;
}

struct ReadinessCheck final {
    bool ready{false};
    std::string cause{};
};

[[nodiscard]] ReadinessCheck confirm_explicit_target(const std::string& target) {
    ReadinessCheck check;
    GstDeviceMonitor* monitor = gst_device_monitor_new();
    if (monitor == nullptr) {
        check.cause = "disabled: requested output '" + target
            + "' cannot be resolved (device monitor unavailable)";
        return check;
    }
    gst_device_monitor_add_filter(monitor, "Audio/Sink", nullptr);
    if (!gst_device_monitor_start(monitor)) {
        gst_object_unref(monitor);
        check.cause = "disabled: requested output '" + target
            + "' cannot be resolved (device monitor failed)";
        return check;
    }
    GList* devices = gst_device_monitor_get_devices(monitor);
    bool matched = false;
    for (GList* item = devices; item != nullptr; item = item->next) {
        GstDevice* device = GST_DEVICE(item->data);
        if (device_matches_target(device, target)) {
            matched = true;
        }
        gst_object_unref(device);
    }
    g_list_free(devices);
    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);
    if (!matched) {
        check.cause = "disabled: requested output '" + target
            + "' not found among the routing backend's outputs";
        return check;
    }
    check.ready = true;
    check.cause = "enabled: requested output '" + target + "' resolved";
    return check;
}

} // namespace

[[nodiscard]] std::string_view to_string(SinkBackend backend) noexcept {
    switch (backend) {
    case SinkBackend::automatic: return "automatic";
    case SinkBackend::pipewire: return "pipewire";
    case SinkBackend::pulse: return "pulse";
    }
    return "unknown";
}

SinkReport probe_sinks() {
    ensure_gstreamer_initialized();
    install_handoff_observer();
    SinkReport report;
    for (const auto& known : kKnownSinks) {
        report.candidates.push_back(probe_candidate(known));
    }
    enable_handoff_thresholds();
    const SinkSelection selection = select_sink_backend(report, SinkBackend::automatic);
    report.selected_element = selection.element;
    report.usable = selection.usable;
    report.cause = selection.cause;
    return report;
}

namespace detail {

bool start_foreign_sink_traffic(std::uint32_t buffers) {
    ensure_gstreamer_initialized();
    install_handoff_observer();
    if (g_foreign_pipeline != nullptr) {
        return true;
    }
    GstElement* pipeline = gst_pipeline_new("aa_foreign_traffic");
    GstElement* source = gst_element_factory_make("audiotestsrc", "foreign_src");
    GstElement* convert = gst_element_factory_make("audioconvert", "foreign_convert");
    GstElement* sink = gst_element_factory_make("pipewiresink", "foreign_sink");
    if (pipeline == nullptr || source == nullptr || convert == nullptr || sink == nullptr) {
        release_elements({pipeline, source, convert, sink});
        return false;
    }
    g_object_set(source, "is-live", TRUE, "num-buffers", static_cast<gint>(buffers), nullptr);
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), source, convert, sink, nullptr);
    if (!gst_element_link_many(source, convert, sink, nullptr)) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }
    g_foreign_pipeline = pipeline;
    return true;
}

void stop_foreign_sink_traffic() noexcept {
    if (g_foreign_pipeline == nullptr) {
        return;
    }
    gst_element_set_state(g_foreign_pipeline, GST_STATE_NULL);
    gst_object_unref(g_foreign_pipeline);
    g_foreign_pipeline = nullptr;
}

std::uint64_t handoff_events_seen() noexcept {
    return g_handoff_events_seen.load(std::memory_order_relaxed);
}

// Test-only queue admission hooks (gate fix 3) - see AudioSinkFactory.hpp for
// the contract. They resolve the seam's sink to the GStreamer implementation
// here inside the adapter zone; no GStreamer type ever crosses the seam.

namespace {
[[nodiscard]] GstSink* gstreamer_sink_of(ObservedAudioSink& sink) noexcept {
    return dynamic_cast<GstSink*>(&sink);
}
} // namespace

bool block_downstream_for_test(ObservedAudioSink& sink) {
    GstSink* gstreamer_sink = gstreamer_sink_of(sink);
    return gstreamer_sink != nullptr && gstreamer_sink->arm_downstream_block();
}

bool wait_downstream_block_entered_for_test(ObservedAudioSink& sink, core::Milliseconds timeout) {
    GstSink* gstreamer_sink = gstreamer_sink_of(sink);
    return gstreamer_sink != nullptr && gstreamer_sink->wait_downstream_block_entered(timeout);
}

void unblock_downstream_for_test(ObservedAudioSink& sink) noexcept {
    GstSink* gstreamer_sink = gstreamer_sink_of(sink);
    if (gstreamer_sink != nullptr) {
        gstreamer_sink->disarm_downstream_block();
    }
}

std::uint64_t prefill_appsrc_for_test(ObservedAudioSink& sink, std::uint64_t target_bytes) {
    GstSink* gstreamer_sink = gstreamer_sink_of(sink);
    return gstreamer_sink != nullptr ? gstreamer_sink->prefill_appsrc(target_bytes) : 0;
}

AppsrcQueueLevel appsrc_queue_level_for_test(ObservedAudioSink& sink) {
    GstSink* gstreamer_sink = gstreamer_sink_of(sink);
    return gstreamer_sink != nullptr ? gstreamer_sink->appsrc_queue_level() : AppsrcQueueLevel{};
}


SinkOpenOutcome open_default_sink(const SinkOpenRequest& request) {
    ensure_gstreamer_initialized();
    install_handoff_observer();

    SinkOpenOutcome outcome;
    outcome.report = probe_sinks();
    outcome.report.requested = request.backend;

    const SinkSelection selection = select_sink_backend(outcome.report, request.backend);
    outcome.report.selected_element = selection.element;
    outcome.report.usable = selection.usable;
    outcome.report.cause = selection.cause;
    if (!selection.usable) {
        // No PipeWire-capable element: recorded cause, degraded typed path.
        return outcome;
    }

    const char* format_name = nullptr;
    switch (request.format.bits_per_sample) {
    case 16: format_name = "S16LE"; break;
    case 24: format_name = "S24LE"; break;
    case 32: format_name = "S32LE"; break;
    default: break;
    }
    if (format_name == nullptr || request.format.channels == 0
        || request.format.sample_rate_hz == 0) {
        outcome.report.usable = false;
        outcome.report.cause = "disabled: unsupported routing format";
        return outcome;
    }

    // Explicit-target policy for the element's OUTPUT target (pipewiresink
    // `target-object`); a pulsesink target is a server address, not a device.
    if (std::string_view{selection.element} == "pipewiresink" && !request.target.empty()) {
        const ReadinessCheck ready = confirm_explicit_target(request.target);
        if (!ready.ready) {
            outcome.report.usable = false;
            outcome.report.cause = ready.cause;
            return outcome;
        }
    }

    GstElement* pipeline = gst_pipeline_new("aa_audio_route");
    GstElement* source = gst_element_factory_make("appsrc", "src");
    GstElement* convert = gst_element_factory_make("audioconvert", "convert");
    GstElement* resample = gst_element_factory_make("audioresample", "resample");
    GstElement* filter = gst_element_factory_make("capsfilter", "filter");
    GstElement* sink = gst_element_factory_make(selection.element.c_str(), "sink");

    GstCaps* caps = gst_caps_new_simple("audio/x-raw",
                                        "format", G_TYPE_STRING, format_name,
                                        "rate", G_TYPE_INT,
                                        static_cast<int>(request.format.sample_rate_hz),
                                        "channels", G_TYPE_INT,
                                        static_cast<int>(request.format.channels),
                                        "layout", G_TYPE_STRING, "interleaved",
                                        nullptr);

    const bool complete = pipeline != nullptr && source != nullptr && convert != nullptr
        && resample != nullptr && filter != nullptr && sink != nullptr && caps != nullptr;
    if (!complete) {
        release_elements({pipeline, source, convert, resample, filter, sink});
        if (caps != nullptr) {
            gst_caps_unref(caps);
        }
        outcome.report.usable = false;
        outcome.report.cause = "full output path could not be constructed; disabled";
        return outcome;
    }

    // Bounded nonblocking enqueue (gate fix 3): block=FALSE + leaky NONE
    // makes gst_app_src_push_buffer() never wait and never drop, and the
    // max-* values below are QUEUE THRESHOLDS ONLY (appsrc keeps accepting
    // past them). The application therefore enforces admission itself in
    // GstSink::write_with_timeout: it stops pushing while the queue cannot
    // hold the complete data+marker pair (64 KiB / 2 buffers).
    gst_app_src_set_max_bytes(GST_APP_SRC(source), kAppsrcMaxBytes);
    gst_app_src_set_max_buffers(GST_APP_SRC(source), kAppsrcMaxBuffers);
    gst_app_src_set_max_time(GST_APP_SRC(source), 0);
    gst_app_src_set_leaky_type(GST_APP_SRC(source), GST_APP_LEAKY_TYPE_NONE);
    gst_app_src_set_emit_signals(GST_APP_SRC(source), FALSE);
    g_object_set(source,
                 "caps", caps,
                 "format", GST_FORMAT_TIME,
                 "is-live", FALSE,
                 "block", FALSE,
                 nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    // Clock-synced render (gate r2 F1): the renderer only hands audio to
    // PipeWire/Pulse from a streaming stream, and stream-relative PTS (see
    // GstSink::chunk_pts) keep the scheduling meaningful. sync=TRUE matches
    // the control pipeline (`gst-launch audiotestsrc is-live ... ! pipewiresink`)
    // whose render handoffs are observable.
    g_object_set(sink, "sync", TRUE, "async", TRUE, nullptr);

    if (!request.target.empty()) {
        // Connection-target override (pulsesink `server`, pipewiresink
        // `target-object`); empty selects the current default output.
        if (std::string_view{selection.element} == "pulsesink") {
            g_object_set(sink, "server", request.target.c_str(), nullptr);
        } else {
            g_object_set(sink, "target-object", request.target.c_str(), nullptr);
        }
    }

    gst_bin_add_many(GST_BIN(pipeline), source, convert, resample, filter, sink, nullptr);
    if (!gst_element_link_many(source, convert, resample, filter, sink, nullptr)) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        outcome.report.usable = false;
        outcome.report.cause = "full output path could not be constructed; disabled";
        return outcome;
    }

    auto handle = std::make_unique<GstSink>(SinkElements{pipeline, source}, request.format,
                                            request.write_timeout, sink,
                                            request.drop_nonempty_at_ingress,
                                            request.drop_nonempty_after_window);
    if (!handle->attach_probe()) {
        handle->release();
        outcome.report.usable = false;
        outcome.report.cause = "full output path could not be constructed; disabled";
        return outcome;
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        handle->release();
        outcome.report.usable = false;
        outcome.report.cause = "full output path could not be constructed or started; disabled";
        return outcome;
    }

    // Route readiness (gate r2 F3): the CHOSEN route must demonstrably accept
    // and hand off a buffer within the startup bound - a connected, streaming
    // output, not a generic provider enumeration. An isolated backend with no
    // linkable output never hands off and is recorded disabled here.
    if (!handle->confirm_route(request.start_timeout)) {
        handle->release();
        outcome.report.usable = false;
        outcome.report.cause =
            "disabled: route readiness not confirmed (the chosen route did not accept and hand "
            "off a probe buffer within the startup bound; no connected streaming output)";
        return outcome;
    }
    handle->reset_observations();
    if (request.drop_nonempty_at_ingress || request.drop_nonempty_after_window) {
        handle->arm_fault_injection();
    }

    GstState state = GST_STATE_NULL;
    GstState pending = GST_STATE_NULL;
    const GstClockTime timeout_ns =
        static_cast<GstClockTime>(request.start_timeout.count) * 1000ULL * 1000ULL;
    (void)gst_element_get_state(pipeline, &state, &pending, timeout_ns);

    outcome.sink = std::move(handle);
    return outcome;
}

} // namespace detail
} // namespace aa::audio
