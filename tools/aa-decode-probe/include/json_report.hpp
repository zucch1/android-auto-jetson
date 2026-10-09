#pragma once

#include "discovery.hpp"
#include "metrics.hpp"
#include "options.hpp"

#include <string>

namespace aa::decode_probe {

std::string format_json_report(const ProbeOptions& options,
                               const DiscoveryResult& discovery,
                               const ProbeMetrics& metrics);

} // namespace aa::decode_probe
