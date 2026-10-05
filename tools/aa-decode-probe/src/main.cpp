#include "discovery.hpp"
#include "json_report.hpp"
#include "metrics.hpp"
#include "options.hpp"
#include "pipeline.hpp"

#include <fstream>
#include <iostream>
#include <gst/gst.h>

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    aa::decode_probe::ProbeOptions options;
    std::string parse_err;
    if (!aa::decode_probe::parse_options(argc, argv, options, parse_err)) {
        std::cerr << "Error: " << parse_err << "\n";
        aa::decode_probe::print_help(argv[0]);

        // Output error JSON if requested
        if (!options.output_json_path.empty()) {
            std::ofstream out(options.output_json_path);
            out << "{\n  \"schema\": \"aa-decode-probe/1\",\n"
                << "  \"pass\": false,\n"
                << "  \"status\": \"ERROR\",\n"
                << "  \"failure_reason\": \"" << parse_err << "\"\n}\n";
        }
        return 1;
    }

    if (options.help) {
        aa::decode_probe::print_help(argv[0]);
        return 0;
    }

    // Decoder discovery
    auto discovery = aa::decode_probe::discover_decoders(options.forced_decoder);

    // Run probe
    auto metrics = aa::decode_probe::run_decode_probe(options, discovery);

    // Format JSON
    std::string json = aa::decode_probe::format_json_report(options, discovery, metrics);

    if (!options.output_json_path.empty()) {
        std::ofstream out(options.output_json_path);
        if (out.is_open()) {
            out << json;
            out.close();
        } else {
            std::cerr << "Warning: Failed to write output JSON to " << options.output_json_path << "\n";
        }
    }

    std::cout << json << std::endl;

    return metrics.pass ? 0 : 2;
}
