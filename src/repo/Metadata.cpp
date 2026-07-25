#include "repo/Metadata.hpp"

#include "repo/Artifact.hpp"

#include <algorithm>
#include <format>

namespace qiln {

namespace {

constexpr std::size_t kMaxVersions = 10000;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

// The text of the first `<tag>...</tag>` at or after `from`, without nested elements; `next` points past it.
std::optional<std::string_view> element(std::string_view xml, std::string_view tag, std::size_t from,
                                        std::size_t& next) {
    std::string open = std::format("<{}>", tag);
    std::string close = std::format("</{}>", tag);
    std::size_t begin = xml.find(open, from);
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    begin += open.size();
    std::size_t end = xml.find(close, begin);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    next = end + close.size();
    return xml.substr(begin, end - begin);
}

} // namespace

std::expected<std::vector<quartz::Version>, std::string> parseMetadata(std::string_view xml, std::string_view name) {
    auto coords = coordinatesOf(name);
    if (!coords) {
        return std::unexpected(std::format("'{}' is not a package name", name));
    }
    std::size_t next = 0;
    if (auto artifact = element(xml, "artifactId", 0, next); artifact && trim(*artifact) != coords->artifact) {
        return std::unexpected(std::format("maven-metadata.xml of {} names artifact '{}'", name, trim(*artifact)));
    }
    if (auto group = element(xml, "groupId", 0, next); group && trim(*group) != coords->group) {
        return std::unexpected(std::format("maven-metadata.xml of {} names group '{}'", name, trim(*group)));
    }
    auto versions = element(xml, "versions", 0, next);
    std::vector<quartz::Version> out;
    if (!versions) {
        return out;
    }
    std::size_t at = 0;
    while (auto v = element(*versions, "version", at, at)) {
        if (out.size() >= kMaxVersions) {
            return std::unexpected(std::format("maven-metadata.xml of {} lists too many versions", name));
        }
        std::string_view text = trim(*v);
        auto parsed = quartz::parseVersion(text);
        if (parsed && quartz::formatVersion(*parsed) == text) {
            out.push_back(*parsed);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string printMetadata(std::string_view name, std::vector<quartz::Version> versions, std::string_view timestamp) {
    Coordinates c = *coordinatesOf(name);
    std::sort(versions.begin(), versions.end());
    versions.erase(std::unique(versions.begin(), versions.end()), versions.end());
    std::string list;
    for (const quartz::Version& v : versions) {
        list += std::format("      <version>{}</version>\n", quartz::formatVersion(v));
    }
    std::string latest = versions.empty() ? std::string() : quartz::formatVersion(versions.back());
    return std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<metadata>\n"
                       "  <groupId>{}</groupId>\n"
                       "  <artifactId>{}</artifactId>\n"
                       "  <versioning>\n"
                       "    <latest>{}</latest>\n"
                       "    <release>{}</release>\n"
                       "    <versions>\n"
                       "{}"
                       "    </versions>\n"
                       "    <lastUpdated>{}</lastUpdated>\n"
                       "  </versioning>\n"
                       "</metadata>\n",
                       c.group, c.artifact, latest, latest, list, timestamp);
}

} // namespace qiln
