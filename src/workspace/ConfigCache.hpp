#pragma once

#include "model/Model.hpp"
#include "support/Hash.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace qiln {

/// What a configuration was computed from: each input (a file, a directory, the Qiln version...) and its hash.
using Fingerprint = std::vector<std::pair<std::string, Hash32>>;

std::vector<std::uint8_t> encodeConfiguration(const Fingerprint& inputs, const WorkspaceModel& model);
/// Nullopt for anything that is not a configuration this version of Qiln wrote.
std::optional<std::pair<Fingerprint, WorkspaceModel>> decodeConfiguration(std::span<const std::uint8_t> bytes);

} // namespace qiln
