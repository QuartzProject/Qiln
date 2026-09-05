#include "repo/Repositories.hpp"

#include "repo/Metadata.hpp"
#include "resolve/Lockfile.hpp"
#include "support/Files.hpp"

#include <cstdio>
#include <format>

namespace qiln {

namespace {

constexpr std::size_t kMaxMetadata = 1u << 20;
constexpr std::size_t kMaxMeta = 64u << 10;
constexpr std::size_t kMaxArchive = 64u << 20;

fs::path cacheRoot() { return qilnHome() / "cache"; }

} // namespace

std::expected<std::vector<quartz::Version>, std::string> Repositories::versions(const std::string& name) {
    if (auto it = versions_.find(name); it != versions_.end()) {
        return it->second;
    }
    if (!coordinatesOf(name)) {
        return std::unexpected(std::format("'{}' is not a package name", name));
    }
    for (std::size_t i = 0; i < locations_.size(); ++i) {
        auto bytes = download(locations_[i], metadataPath(name), kMaxMetadata);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        if (!*bytes) {
            continue;
        }
        auto list = parseMetadata(textOf(**bytes), name);
        if (!list) {
            return std::unexpected(std::format("{}: {}", locations_[i].display, list.error()));
        }
        if (list->empty()) {
            return std::unexpected(std::format("{} has {} but lists no major.minor.patch version of it",
                                               locations_[i].display, name));
        }
        home_[name] = i;
        return versions_[name] = std::move(*list);
    }
    return versions_[name] = {};
}

std::expected<Meta, std::string> Repositories::meta(const std::string& name, const quartz::Version& version) {
    std::string path = versionPrefix(name, version) + ".qzmeta";
    fs::path cached = cacheRoot() / "meta" / path;
    if (auto text = readText(cached)) {
        if (auto m = parseMeta(*text, name, version)) {
            return m;
        }
    }
    auto home = home_.find(name);
    if (home == home_.end()) {
        if (auto v = versions(name); !v) {
            return std::unexpected(v.error());
        }
        home = home_.find(name);
        if (home == home_.end()) {
            return std::unexpected(std::format("{} is not in any repository", name));
        }
    }
    auto bytes = download(locations_[home->second], path, kMaxMeta);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    if (!*bytes) {
        return std::unexpected(std::format("{} lists {} {} but has no {}", locations_[home->second].display, name,
                                           quartz::formatVersion(version), path));
    }
    auto m = parseMeta(textOf(**bytes), name, version);
    if (!m) {
        return std::unexpected(m.error());
    }
    // Versions never change, so a verified description is kept for good; a failure here only costs a download.
    (void)writeAtomically(cached, **bytes);
    return m;
}

std::expected<std::vector<std::uint8_t>, std::string> Repositories::artifact(const Locked& p) {
    std::string ext(archiveExtension(p.kind));
    fs::path cached = cacheRoot() / "artifacts" / (toHex(p.checksum) + ext);
    if (auto bytes = readLimited(cached, kMaxArchive); bytes && *bytes && blake2b256(**bytes) == p.checksum) {
        return std::move(**bytes);
    }
    std::string label = std::format("{} {}", p.name, quartz::formatVersion(p.version));
    std::string path = versionPrefix(p.name, p.version) + ext;
    for (const Location& loc : locations_) {
        auto bytes = download(loc, path, kMaxArchive);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        if (!*bytes) {
            continue;
        }
        if (blake2b256(**bytes) != p.checksum) {
            return std::unexpected(std::format("checksum mismatch for {} from {}: the download is not what was "
                                               "published and locked",
                                               label, loc.display));
        }
        if (auto w = writeAtomically(cached, **bytes); !w) {
            std::fprintf(stderr, "warning: %s\n", w.error().c_str());
        }
        return std::move(**bytes);
    }
    return std::unexpected(std::format("{} is locked but no repository has {}", label, path));
}

} // namespace qiln
