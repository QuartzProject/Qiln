#include "repo/Artifact.hpp"

#include <format>

namespace qiln {

std::string_view kindName(ArtifactKind kind) {
    switch (kind) {
    case ArtifactKind::Library:
        return "library";
    case ArtifactKind::Application:
        return "application";
    case ArtifactKind::Plugin:
        return "plugin";
    case ArtifactKind::BuildPlugin:
        return "build-plugin";
    }
    return "library";
}

std::expected<ArtifactKind, std::string> parseKind(std::string_view name) {
    for (ArtifactKind k :
         {ArtifactKind::Library, ArtifactKind::Application, ArtifactKind::Plugin, ArtifactKind::BuildPlugin}) {
        if (kindName(k) == name) {
            return k;
        }
    }
    return std::unexpected(std::format("unknown kind '{}' (library, application, plugin or build-plugin)", name));
}

std::string_view archiveExtension(ArtifactKind kind) {
    return kind == ArtifactKind::Library || kind == ArtifactKind::BuildPlugin ? ".qzs" : ".qzb";
}

std::string formatDependency(const Dependency& d) { return d.name + "@" + d.range; }

std::expected<Dependency, std::string> parseDependency(std::string_view text) {
    std::size_t at = text.find('@');
    if (at == std::string_view::npos) {
        return std::unexpected(std::format("'{}' is not name@range", text));
    }
    Dependency d{std::string(text.substr(0, at)), std::string(text.substr(at + 1))};
    if (!quartz::isPackageName(d.name)) {
        return std::unexpected(std::format("'{}' is not a package name like org.example.text", d.name));
    }
    if (auto ok = quartz::satisfies(quartz::Version{}, d.range); !ok) {
        return std::unexpected(std::format("'{}' is not a version range: {}", d.range, ok.error()));
    }
    return d;
}

std::optional<Coordinates> coordinatesOf(std::string_view name) {
    if (name.size() > 200 || !quartz::isPackageName(name)) {
        return std::nullopt;
    }
    std::size_t dot = name.rfind('.');
    return Coordinates{std::string(name.substr(0, dot)), std::string(name.substr(dot + 1))};
}

namespace {

std::string groupPath(const Coordinates& c) {
    std::string out = c.group;
    for (char& ch : out) {
        if (ch == '.') {
            ch = '/';
        }
    }
    return out;
}

} // namespace

std::string metadataPath(std::string_view name) {
    Coordinates c = *coordinatesOf(name);
    return std::format("{}/{}/maven-metadata.xml", groupPath(c), c.artifact);
}

std::string versionPrefix(std::string_view name, const quartz::Version& version) {
    Coordinates c = *coordinatesOf(name);
    std::string v = quartz::formatVersion(version);
    return std::format("{}/{}/{}/{}-{}", groupPath(c), c.artifact, v, c.artifact, v);
}

} // namespace qiln
