#pragma once

#include "resolve/Lockfile.hpp"

#include <expected>
#include <functional>
#include <string>
#include <vector>

namespace qiln {

struct Meta;

/// Where the resolver learns about packages; `Repositories` in a build, a map in the tests.
struct PackageSource {
    std::function<std::expected<std::vector<quartz::Version>, std::string>(const std::string& name)> versions;
    std::function<std::expected<Meta, std::string>(const std::string& name, const quartz::Version& version)> meta;
};

struct ResolveOptions {
    const LockGraph* previous = nullptr; // versions kept while still in range, and keys trusted on first use
    bool update = false;                 // move to the newest versions, keeping only the previous keys
    bool allowKeyChange = false;
};

/// The highest version each range accepts; a conflict rules out the version that caused it and starts again.
std::expected<LockGraph, std::string> resolve(const std::vector<Requirement>& roots, const PackageSource& source,
                                              const ResolveOptions& options = {});

} // namespace qiln
