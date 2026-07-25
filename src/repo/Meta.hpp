#pragma once

#include "repo/Artifact.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace qiln {

/// The `.qzmeta` beside each artifact: what Maven cannot say, signed by the publisher.
struct Meta {
    std::string name;
    quartz::Version version;
    ArtifactKind kind = ArtifactKind::Library;
    std::string quartz;             // version of the compiler that checked it before publishing
    std::uint32_t qzbFormat = 0;    // `.qzb` format of a module artifact; 0 for sources
    std::vector<Dependency> dependencies; // sorted by name
    std::vector<std::string> capabilities;
    std::vector<std::string> interfaces; // host interfaces it compiles against, by file name
    std::vector<std::string> exports;
    std::string archive;            // file name of the artifact in the same directory
    Hash32 checksum{};              // BLAKE2b-256 of the artifact
    PublicKey key{};
    Signature signature{};
};

/// The canonical text the publisher signs: every field above except the signature, one per line.
std::string signedMessage(const Meta& m);
void signMeta(Meta& m, const Seed& seed);
[[nodiscard]] bool verifyMeta(const Meta& m);

/// Parses and checks a `.qzmeta`: its name and version must be the ones asked for and its signature must hold.
std::expected<Meta, std::string> parseMeta(std::string_view text, std::string_view name,
                                           const quartz::Version& version);
std::string printMeta(const Meta& m);

} // namespace qiln
