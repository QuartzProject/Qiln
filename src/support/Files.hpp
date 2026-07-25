#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qiln {

namespace fs = std::filesystem;

std::optional<std::string> readText(const fs::path& path);
/// The file's bytes; nullopt when it does not exist, an error when it is larger than `limit`.
std::expected<std::optional<std::vector<std::uint8_t>>, std::string> readLimited(const fs::path& path,
                                                                                 std::size_t limit);
/// Writes through a temporary file and a rename, so readers never see half a file.
std::expected<void, std::string> writeAtomically(const fs::path& path, std::span<const std::uint8_t> bytes);
std::expected<void, std::string> writeAtomically(const fs::path& path, std::string_view text);

/// `$QILN_HOME`, else `~/.qiln` (`%USERPROFILE%\.qiln` on Windows).
fs::path qilnHome();

/// True for a relative `/` separated path of plain segments, without `.`, `..`, empty or hidden parts.
bool safeRelativePath(std::string_view path);

/// Files under `dir` with one of `extensions`, as `/` separated paths relative to `base`, sorted.
std::expected<std::vector<std::string>, std::string> listFiles(const fs::path& base, const fs::path& dir,
                                                              std::span<const std::string_view> extensions);

std::span<const std::uint8_t> bytesOf(std::string_view text);
std::string_view textOf(std::span<const std::uint8_t> bytes);

} // namespace qiln
