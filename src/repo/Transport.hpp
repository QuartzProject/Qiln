#pragma once

#include "repo/Credentials.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qiln {

/// A repository as a build script gives it: a directory, `file://` or `https://`.
struct Location {
    std::string url;             // `https://...` without a trailing slash, empty for a directory
    std::filesystem::path local; // the directory of a local repository
    std::string display;         // what to show in messages
    bool insecure = false;       // plain http to the loopback address, allowed by --allow-insecure-localhost
    std::optional<Credentials> credentials;

    [[nodiscard]] bool remote() const { return !url.empty(); }
};

/// `allowInsecureLocalhost` accepts `http://` to 127.0.0.1, [::1] or localhost, for a repository started for a test.
std::expected<Location, std::string> locate(std::string_view spec, const std::filesystem::path& relativeTo,
                                            bool allowInsecureLocalhost = false);

/// A file of the repository; nullopt when it does not exist. Larger than `limit` bytes is an error.
std::expected<std::optional<std::vector<std::uint8_t>>, std::string>
download(const Location& repo, const std::string& path, std::size_t limit);

/// Writes a file of the repository (an HTTP PUT for a remote one); `fresh` refuses to replace an existing file.
std::expected<void, std::string> store(const Location& repo, const std::string& path,
                                       std::span<const std::uint8_t> bytes, bool fresh);

} // namespace qiln
