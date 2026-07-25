#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qiln {

/// A repository as a build script gives it: a directory, `file://` or `https://`.
struct Location {
    std::string url;             // `https://...` without a trailing slash, empty for a directory
    std::filesystem::path local; // the directory of a local repository
    std::string display;         // what to show in messages

    [[nodiscard]] bool remote() const { return !url.empty(); }
};

std::expected<Location, std::string> locate(std::string_view spec, const std::filesystem::path& relativeTo);

/// A file of the repository; nullopt when it does not exist. Larger than `limit` bytes is an error.
std::expected<std::optional<std::vector<std::uint8_t>>, std::string> download(const Location& repo,
                                                                              const std::string& path,
                                                                              std::size_t limit);

} // namespace qiln
