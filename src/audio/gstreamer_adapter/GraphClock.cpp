// SPDX-License-Identifier: GPL-3.0-or-later
#include "GraphClock.hpp"
#include "GraphCycles.hpp"

#include <pipewire/impl.h>
#include <pipewire/extensions/profiler.h>
#include <spa/pod/parser.h>
#include <spa/param/profiler.h>
#include <cstring>
#include <charconv>
#include <algorithm>
#include <mutex>

namespace aa::audio::detail {
struct GraphClock::Observer {
    GraphClock& clock;
    pw_thread_loop* loop{};
    pw_context* context{};
    pw_core* core{};
    pw_registry* registry{};
    pw_proxy* profiler{};
    spa_hook globals{}, profiles{}, node_events{};
    uint32_t stream_id{PW_ID_ANY}, sink_id{PW_ID_ANY}, profiler_id{PW_ID_ANY};
    pw_node* sink{};
    bool sink_running{};
    GraphCycles cycles;
    bool started{};

    static void profile(void* data, const spa_pod* pod) {
        auto& self = *static_cast<Observer*>(data);
        void* entry;
        SPA_POD_STRUCT_FOREACH(pod, entry) {
            auto* object = static_cast<spa_pod*>(entry);
            if (!spa_pod_is_object_type(object, SPA_TYPE_OBJECT_Profiler)) { continue; }
            spa_fraction rate{};
            std::int64_t position{}, duration{}, ns{};
            int status = -1;
            spa_pod_prop* field;
            SPA_POD_OBJECT_FOREACH(reinterpret_cast<spa_pod_object*>(object), field) {
                if (field->key == SPA_PROFILER_clock) {
                    int32_t flags{}, id{};
                    const char* driver{};
                    if (spa_pod_parse_struct(&field->value, SPA_POD_Int(&flags), SPA_POD_Int(&id),
                        SPA_POD_String(&driver), SPA_POD_Long(&ns), SPA_POD_Fraction(&rate),
                        SPA_POD_Long(&position), SPA_POD_Long(&duration)) < 0) { duration = 0; }
                } else if (field->key == SPA_PROFILER_followerBlock) {
                    int32_t id{}, state{};
                    const char* name{};
                    int64_t previous{}, signal{}, awake{}, finish{};
                    if (spa_pod_parse_struct(&field->value, SPA_POD_Int(&id), SPA_POD_String(&name),
                        SPA_POD_Long(&previous), SPA_POD_Long(&signal), SPA_POD_Long(&awake),
                        SPA_POD_Long(&finish), SPA_POD_Int(&state)) >= 0 && static_cast<uint32_t>(id) == self.stream_id && self.clock.name == name) {
                        status = state;
                    }
                }
            }
            if (status < 0 || duration <= 0 || duration > 65536 || rate.num != 1 || !rate.denom) { continue; }
            if (position < 0 || position > INT64_MAX - duration) { continue; }
            const auto change = self.cycles.observe(position, duration, rate.denom, status, ns);
            if (change.discontinuity) { ++self.clock.disruptions; }
            if (change.drift_ns) { self.clock.drift += change.drift_ns; }
            if (change.held_ns) { self.clock.held += change.held_ns; }
            self.clock.available = true;
        }
    }
    static void sink_info(void* data, const pw_node_info* info) {
        auto& self = *static_cast<Observer*>(data);
        if (!(info->change_mask & PW_NODE_CHANGE_MASK_STATE)) { return; }
        if (info->state == PW_NODE_STATE_RUNNING) { self.sink_running = true; }
        if (info->state == PW_NODE_STATE_SUSPENDED && self.sink_running) {
            self.sink_running = false;
            ++self.clock.suspends;
        }
    }
    static uint32_t node_id(const spa_dict* props, const char* key) {
        const char* value = props ? spa_dict_lookup(props, key) : nullptr;
        uint32_t id{};
        if (!value) { return PW_ID_ANY; }
        auto result = std::from_chars(value, value + std::strlen(value), id);
        return result.ec == std::errc{} ? id : PW_ID_ANY;
    }
    void detach_sink() {
        if (sink) {
            spa_hook_remove(&node_events);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(sink));
            sink = nullptr;
        }
        sink_id = PW_ID_ANY;
        sink_running = false;
    }
    void attach_sink(uint32_t id) {
        if (id == PW_ID_ANY || id == sink_id) { return; }
        detach_sink();
        sink = static_cast<pw_node*>(pw_registry_bind(registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (!sink) { return; }
        sink_id = id;
        static const pw_node_events events{PW_VERSION_NODE_EVENTS, sink_info, nullptr};
        pw_node_add_listener(sink, &node_events, &events, this);
    }
    static void removed(void* data, uint32_t id) {
        auto& self = *static_cast<Observer*>(data);
        if (id == self.sink_id) { self.detach_sink(); }
        if (id == self.stream_id || id == self.profiler_id) {
            self.clock.available = false;
            self.cycles = {};
            if (id == self.stream_id) { self.stream_id = PW_ID_ANY; }
            if (id == self.profiler_id) {
                pw_proxy_destroy(self.profiler);
                self.profiler = nullptr;
                self.profiler_id = PW_ID_ANY;
            }
        }
    }
    static void global(void* data, uint32_t id, uint32_t, const char* type, uint32_t version, const spa_dict* props) {
        auto& self = *static_cast<Observer*>(data);
        if (props && !std::strcmp(type, PW_TYPE_INTERFACE_Node)) {
            const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            if (name && self.clock.name == name) { self.stream_id = id; }
        }
        if (self.stream_id != PW_ID_ANY && !std::strcmp(type, PW_TYPE_INTERFACE_Link)
            && node_id(props, PW_KEY_LINK_OUTPUT_NODE) == self.stream_id) {
            self.attach_sink(node_id(props, PW_KEY_LINK_INPUT_NODE));
        }
        if (self.profiler || std::strcmp(type, PW_TYPE_INTERFACE_Profiler)) { return; }
        self.profiler = static_cast<pw_proxy*>(pw_registry_bind(self.registry, id, type,
            std::min(version, static_cast<uint32_t>(PW_VERSION_PROFILER)), 0));
        static const pw_profiler_events events{PW_VERSION_PROFILER_EVENTS, profile};
        if (self.profiler) {
            self.profiler_id = id;
            pw_proxy_add_object_listener(self.profiler, &self.profiles, &events, &self);
        }
    }
    explicit Observer(GraphClock& owner) : clock(owner) {
        static std::once_flag init;
        std::call_once(init, [] { pw_init(nullptr, nullptr); });
        loop = pw_thread_loop_new("aa-graph-clock", nullptr);
        if (!loop) { return; }
        context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
        if (!context) { return; }
        pw_context_load_module(context, PW_EXTENSION_MODULE_PROFILER, nullptr, nullptr);
        core = pw_context_connect(context, nullptr, 0);
        if (!core) { return; }
        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        if (!registry) { return; }
        static const pw_registry_events events{PW_VERSION_REGISTRY_EVENTS, global, removed};
        pw_registry_add_listener(registry, &globals, &events, this);
        started = pw_thread_loop_start(loop) >= 0;
    }
    ~Observer() {
        if (started) { pw_thread_loop_stop(loop); }
        detach_sink();
        if (profiler) { pw_proxy_destroy(profiler); }
        if (registry) { pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry)); }
        if (core) { pw_core_disconnect(core); }
        if (context) { pw_context_destroy(context); }
        if (loop) { pw_thread_loop_destroy(loop); }
    }
};

GraphClock::GraphClock(std::string node) : name(std::move(node)), observer_(std::make_unique<Observer>(*this)) {}
GraphClock::~GraphClock() = default;
}
