#pragma once

#include "support/Hash.hpp"

#include <quartz/Package.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace qiln {

/// What a package is published as: sources (`.qzs`) compiled into users, or a signed module (`.qzb`).
enum class ArtifactKind : std::uint8_t { Library, Application, Plugin, BuildPlugin };

std::string_view kindName(ArtifactKind kind);
std::expected<ArtifactKind, std::string> parseKind(std::string_view name);
/// `.qzs` for libraries and build plugins, `.qzb` for host plugins and applications.
std::string_view archiveExtension(ArtifactKind kind);

struct Dependency {
    std::string name;
    std::string range;
    friend bool operator==(const Dependency&, const Dependency&) = default;
};

/// `name@range`, the form used in `.qzmeta`, `qiln.lock` and error messages.
std::string formatDependency(const Dependency& d);
std::expected<Dependency, std::string> parseDependency(std::string_view text);

/// Maven coordinates: `org.example.greeter` is group `org.example`, artifact `greeter`.
struct Coordinates {
    std::string group;
    std::string artifact;
};

/// Nullopt unless `name` is a Quartz package name (two or more lower case segments).
std::optional<Coordinates> coordinatesOf(std::string_view name);
/// `org/example/greeter/maven-metadata.xml`.
std::string metadataPath(std::string_view name);
/// `org/example/greeter/1.0.0/greeter-1.0.0` without an extension; every file of a version starts with it.
std::string versionPrefix(std::string_view name, const quartz::Version& version);

} // namespace qiln
