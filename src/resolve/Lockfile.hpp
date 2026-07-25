#pragma once

#include "repo/Artifact.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace qiln {

/// One package exactly as a build uses it.
struct Locked {
    std::string name;
    quartz::Version version;
    ArtifactKind kind = ArtifactKind::Library;
    std::vector<Dependency> dependencies;
    Hash32 checksum{};
    PublicKey key{};
};

/// Packages in load order: every dependency before what needs it.
struct LockGraph {
    std::vector<Locked> packages;
    [[nodiscard]] const Locked* find(std::string_view name) const;
};

/// `qiln.lock`: the packages the projects compile in, and the build plugins with their libraries.
struct Lockfile {
    LockGraph packages;
    LockGraph plugins;
};

std::expected<Lockfile, std::string> parseLock(std::string_view text);
std::string printLock(const Lockfile& lock);

/// A range somebody asks for; `by` names who, for conflict messages.
struct Requirement {
    std::string name;
    std::string range;
    std::string by;
};

/// Whether `graph` still satisfies every root requirement and every range between its own packages.
std::expected<void, std::string> graphCovers(const LockGraph& graph, const std::vector<Requirement>& roots);

} // namespace qiln
