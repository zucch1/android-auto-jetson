// SPDX-License-Identifier: GPL-3.0-or-later
// Regenerates the committed pre-28 conformance fixture files
// (tests/replay/fixtures/conformance/*.json) from the independent wire spec.
// Usage: aa_conformance_fixture_write <output-dir>
// The payload bytes come from conformance_wire_spec.hpp (hand-derived); this
// tool only wraps them in the task-20 aa-replay-fixture-v1 envelope.

#include "conformance_fixture_builder.hpp"

#include <fstream>
#include <iostream>
#include <string>

namespace {

int write_one(const std::string& dir, const std::string& name,
              conformancewire::Filler filler) {
    auto exported = conformancewire::build(filler);
    if (!exported) {
        std::cerr << name << ": export rejected ("
                  << aa::to_string(exported.error().code()) << ")\n";
        return 1;
    }
    const std::string path = dir + "/" + name + ".json";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << path << ": cannot open for write\n";
        return 1;
    }
    for (const std::byte byte : exported.value().file_bytes) {
        out.put(static_cast<char>(byte));
    }
    out.close();
    std::cout << name << ".json sha256=" << exported.value().sha256_hex << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: aa_conformance_fixture_write <output-dir>\n";
        return 2;
    }
    const std::string dir = argv[1];
    int failures = 0;
    failures += write_one(dir, "conformance-control", &conformancewire::fill_control);
    failures += write_one(dir, "conformance-media", &conformancewire::fill_media);
    failures += write_one(dir, "conformance-uiconfig", &conformancewire::fill_uiconfig);
    return failures == 0 ? 0 : 1;
}
