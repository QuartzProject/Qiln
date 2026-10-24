#pragma once

#include "support/Hash.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace qiln {

/// What a task last ran from and what it wrote: its `build/.qiln/<task>.stamp`.
struct Stamp {
    std::vector<std::pair<std::string, Hash32>> inputs;  // an id the runner knows how to hash again, and its hash
    std::vector<std::pair<std::string, Hash32>> outputs; // files relative to the build directory
};

std::vector<std::uint8_t> encodeStamp(const Stamp& stamp);
/// Nullopt for anything that is not a stamp this version of Qiln wrote.
std::optional<Stamp> decodeStamp(std::span<const std::uint8_t> bytes);

} // namespace qiln
