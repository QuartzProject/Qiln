#pragma once

#include <quartz/Package.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace qiln {

/// The versions a `maven-metadata.xml` lists, sorted, ignoring the ones that are not `major.minor.patch`.
std::expected<std::vector<quartz::Version>, std::string> parseMetadata(std::string_view xml, std::string_view name);
/// A `maven-metadata.xml` listing `versions`; `timestamp` is `yyyyMMddHHmmss` in UTC.
std::string printMetadata(std::string_view name, std::vector<quartz::Version> versions, std::string_view timestamp);

} // namespace qiln
