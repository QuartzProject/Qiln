#include "repo/Publish.hpp"

#include "repo/Metadata.hpp"
#include "support/Files.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace qiln {

namespace {

constexpr std::size_t kMaxMetadata = 1u << 20;

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
std::expected<void, std::string> writeWithSums(const fs::path& path, std::span<const std::uint8_t> bytes) {
    if (auto w = writeAtomically(path, bytes); !w) {
        return w;
    }
    fs::path sha256 = path;
    sha256 += ".sha256";
    fs::path sha512 = path;
    sha512 += ".sha512";
    if (auto w = writeAtomically(sha256, sha256Hex(bytes)); !w) {
        return w;
    }
    return writeAtomically(sha512, sha512Hex(bytes));
}

} // namespace

std::string mavenTimestamp() {
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y%m%d%H%M%S}", now);
}

std::expected<std::string, std::string> publish(const Location& repo, Publication& pub, const Seed& seed) {
    if (repo.remote()) {
        return std::unexpected(std::format("publishing over https is not supported yet; publish to a directory and "
                                           "upload it to {}",
                                           repo.url));
    }
    Meta& m = pub.meta;
    std::string version = quartz::formatVersion(m.version);
    fs::path metadataFile = repo.local / metadataPath(m.name);
    std::vector<quartz::Version> versions;
    if (auto bytes = readLimited(metadataFile, kMaxMetadata); bytes && *bytes) {
        auto parsed = parseMetadata(textOf(**bytes), m.name);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        versions = std::move(*parsed);
    } else if (!bytes) {
        return std::unexpected(bytes.error());
    }
    fs::path prefix = repo.local / versionPrefix(m.name, m.version);
    fs::path metaFile = prefix;
    metaFile += ".qzmeta";
    std::error_code ec;
    if (std::find(versions.begin(), versions.end(), m.version) != versions.end() || fs::exists(metaFile, ec)) {
        return std::unexpected(
            std::format("{} {} is already published; versions never change, publish a new one", m.name, version));
    }
    m.checksum = blake2b256(pub.artifact);
    signMeta(m, seed);
    fs::path artifactFile = prefix;
    artifactFile += archiveExtension(m.kind);
    fs::path pomFile = prefix;
    pomFile += ".pom";
    std::string metaText = printMeta(m);
    std::string pomText = pom(m, pub.description);
    for (auto [path, bytes] : {std::pair{&artifactFile, std::span<const std::uint8_t>(pub.artifact)},
                               std::pair{&metaFile, bytesOf(metaText)}, std::pair{&pomFile, bytesOf(pomText)}}) {
        if (auto w = writeWithSums(*path, bytes); !w) {
            return std::unexpected(w.error());
        }
    }
    versions.push_back(m.version);
    std::string listing = printMetadata(m.name, versions, mavenTimestamp());
    if (auto w = writeWithSums(metadataFile, bytesOf(listing)); !w) {
        return std::unexpected(w.error());
    }
    return std::format("published {} {} ({}, {} bytes, blake2b {}) to {}", m.name, version, kindName(m.kind),
                       pub.artifact.size(), toHex(m.checksum).substr(0, 16), repo.display);
}

} // namespace qiln
