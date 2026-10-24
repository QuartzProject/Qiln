#pragma once

#include "repo/Meta.hpp"
#include "repo/Transport.hpp"

#include <expected>
#include <string>
#include <vector>

namespace qiln {

/// One version to publish: its description (unsigned) and its artifact.
struct Publication {
    Meta meta;
    std::string description;
    std::vector<std::uint8_t> artifact;
};

/// Writes a version into a repository (a directory, or over HTTP PUT); the listing goes last, so no reader sees a
/// version without its files.
std::expected<std::string, std::string> publish(const Location& repo, Publication& pub, const Seed& seed);

/// The current time as `yyyyMMddHHmmss` in UTC, as `maven-metadata.xml` wants it.
std::string mavenTimestamp();

} // namespace qiln
