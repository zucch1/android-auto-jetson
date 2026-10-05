#pragma once

#include "discovery.hpp"
#include "metrics.hpp"
#include "options.hpp"

namespace aa::decode_probe {

ProbeMetrics run_decode_probe(const ProbeOptions& options, const DiscoveryResult& discovery);

} // namespace aa::decode_probe
