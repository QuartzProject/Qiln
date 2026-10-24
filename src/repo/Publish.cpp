#include "repo/Publish.hpp"

#include "repo/Metadata.hpp"
#include "support/Files.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace qiln {

namespace {

constexpr std::size_t kMaxMetadata = 1u << 20;
constexpr std::size_t kMaxMeta = 64u << 10;

std::string xmlEscape(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string pom(const Meta& m, std::string_view description) {
    Coordinates c = *coordinatesOf(m.name);
    std::string ext(archiveExtension(m.kind).substr(1));
    return std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<project xmlns=\"http://maven.apache.org/POM/4.0.0\">\n"
                       "  <modelVersion>4.0.0</modelVersion>\n"
                       "  <groupId>{}</groupId>\n"
                       "  <artifactId>{}</artifactId>\n"
                       "  <version>{}</version>\n"
                       "  <packaging>{}</packaging>\n"
                       "  <description>{}</description>\n"
                       "</project>\n",
                       c.group, c.artifact, quartz::formatVersion(m.version), ext, xmlEscape(description));
}

// A file plus the `.sha256` and `.sha512` Maven tools look for beside it.
std::expected<void, std::string> storeWithSums(const Location& repo, const std::string& path,
                                               std::span<const std::uint8_t> bytes, bool fresh) {
    if (auto w = store(repo, path, bytes, fresh); !w) {
        return w;
    }
    if (auto w = store(repo, path + ".sha256", bytesOf(sha256Hex(bytes)), fresh); !w) {
        return w;
    }
    return store(repo, path + ".sha512", bytesOf(sha512Hex(bytes)), fresh);
}

} // namespace

std::string mavenTimestamp() {
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y%m%d%H%M%S}", now);
}

std::expected<std::string, std::string> publish(const Location& repo, Publication& pub, const Seed& seed) {
    Meta& m = pub.meta;
    std::string version = quartz::formatVersion(m.version);
    std::string metadataFile = metadataPath(m.name);
    std::vector<quartz::Version> versions;
    auto listing = download(repo, metadataFile, kMaxMetadata);
    if (!listing) {
        return std::unexpected(listing.error());
    }
    if (*listing) {
        auto parsed = parseMetadata(textOf(**listing), m.name);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        versions = std::move(*parsed);
    }
    std::string prefix = versionPrefix(m.name, m.version);
    auto described = download(repo, prefix + ".qzmeta", kMaxMeta);
    if (!described) {
        return std::unexpected(described.error());
    }
    if (std::find(versions.begin(), versions.end(), m.version) != versions.end() || *described) {
        return std::unexpected(
            std::format("{} {} is already published; versions never change, publish a new one", m.name, version));
    }
    m.checksum = blake2b256(pub.artifact);
    signMeta(m, seed);
    std::string metaText = printMeta(m);
    std::string pomText = pom(m, pub.description);
    std::pair<std::string, std::span<const std::uint8_t>> files[] = {
        {prefix + std::string(archiveExtension(m.kind)), pub.artifact},
        {prefix + ".qzmeta", bytesOf(metaText)},
        {prefix + ".pom", bytesOf(pomText)},
    };
    for (const auto& [path, bytes] : files) {
        if (auto w = storeWithSums(repo, path, bytes, true); !w) {
            return std::unexpected(w.error());
        }
    }
    versions.push_back(m.version);
    std::string text = printMetadata(m.name, versions, mavenTimestamp());
    if (auto w = storeWithSums(repo, metadataFile, bytesOf(text), false); !w) {
        return std::unexpected(w.error());
    }
    return std::format("published {} {} ({}, {} bytes, blake2b {}) to {}", m.name, version, kindName(m.kind),
                       pub.artifact.size(), toHex(m.checksum).substr(0, 16), repo.display);
}

} // namespace qiln
