#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qiln {

/// One file of a source archive.
struct SourceFile {
    std::string path; // relative, `/` separated
    std::string text;
};

/// The `.qzs` format of the Quartz core (QZSA 1): `package.toml` and every `.qz`/`.qzi`, sorted, in one zstd frame.
std::expected<std::vector<std::uint8_t>, std::string> packSources(std::vector<SourceFile> files);
std::expected<std::vector<SourceFile>, std::string> unpackSources(std::span<const std::uint8_t> archive);
/// Whether a relative path may appear in a source archive.
[[nodiscard]] bool archivablePath(std::string_view path);

} // namespace qiln
