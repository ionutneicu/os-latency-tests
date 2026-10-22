// olt/preflight.hpp - checks for conditions that make results noisy.
#pragma once

#include <string>
#include <vector>

#include "olt/benchmark.hpp"

namespace olt {

struct PreflightInput {
    bool memory_sweep_selected = false;
};

// Human-readable warnings, empty when the machine looks quiet and well set up.
std::vector<std::string> preflight_checks(const Config& cfg, const PreflightInput& in);

}  // namespace olt
