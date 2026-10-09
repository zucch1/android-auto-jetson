#include "discovery.hpp"

#include <gst/gst.h>
#include <iostream>

namespace aa::decode_probe {

static bool test_decoder_viable(const std::string& name, std::string& note) {
    GstElementFactory* factory = gst_element_factory_find(name.c_str());
    if (!factory) {
        note = "Factory not found";
        return false;
    }
    gst_object_unref(factory);

    GstElement* elem = gst_element_factory_make(name.c_str(), "probe_elem");
    if (!elem) {
        note = "Failed to instantiate element";
        return false;
    }

    // Try state change NULL -> READY
    GstStateChangeReturn ret = gst_element_set_state(elem, GST_STATE_READY);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        note = "Failed to transition to GST_STATE_READY";
        gst_element_set_state(elem, GST_STATE_NULL);
        gst_object_unref(elem);
        return false;
    }

    gst_element_set_state(elem, GST_STATE_NULL);
    gst_object_unref(elem);
    note = "Instantiated and transitioned to READY successfully";
    return true;
}

DiscoveryResult discover_decoders(const std::string& forced_decoder) {
    DiscoveryResult result;

    const std::vector<std::pair<std::string, bool>> known_candidates = {
        {"nvv4l2decoder", true},
        {"openh264dec",   false},
        {"avdec_h264",    false},
        {"nvh264dec",     true}
    };

    if (!forced_decoder.empty()) {
        DecoderCandidate cand;
        cand.element_name = forced_decoder;
        cand.is_hardware = false;
        for (const auto& [name, is_hw] : known_candidates) {
            if (forced_decoder == name) cand.is_hardware = is_hw;
        }
        GstElementFactory* factory = gst_element_factory_find(forced_decoder.c_str());
        cand.available = factory != nullptr;
        if (factory) gst_object_unref(factory);
        std::string note;
        cand.viable = cand.available && test_decoder_viable(forced_decoder, note);
        cand.notes = note;
        result.candidates.push_back(cand);
        if (cand.viable) {
            result.selected_decoder = cand.element_name;
            result.is_hardware_selected = cand.is_hardware;
        }
        return result;
    }

    for (const auto& [name, is_hw] : known_candidates) {
        DecoderCandidate cand;
        cand.element_name = name;
        cand.is_hardware = is_hw;
        GstElementFactory* fact = gst_element_factory_find(name.c_str());
        cand.available = (fact != nullptr);
        if (fact) {
            gst_object_unref(fact);
            std::string note;
            cand.viable = test_decoder_viable(name, note);
            cand.notes = note;
        } else {
            cand.viable = false;
            cand.notes = "Element not found in GStreamer registry";
        }
        result.candidates.push_back(cand);
    }

    // Selection policy:
    // 1. First viable hardware decoder (if any)
    // 2. Otherwise first viable software decoder
    for (const auto& c : result.candidates) {
        if (c.is_hardware && c.viable) {
            result.selected_decoder = c.element_name;
            result.is_hardware_selected = true;
            return result;
        }
    }

    for (const auto& c : result.candidates) {
        if (!c.is_hardware && c.viable) {
            result.selected_decoder = c.element_name;
            result.is_hardware_selected = false;
            return result;
        }
    }

    return result;
}

} // namespace aa::decode_probe
