#include "repo/Meta.hpp"

#include "support/Toml.hpp"

#include <format>

namespace qiln {

namespace {

constexpr std::int64_t kFormat = 1;
constexpr std::size_t kMaxList = 256;

using Section = toml::Section;

std::expected<std::string, std::string> text(const Section& s, std::string_view key) {
    const toml::Entry* e = s.find(key);
    if (e == nullptr || e->kind != toml::Entry::Kind::String) {
        return std::unexpected(std::format("'{}' needs a string", key));
    }
    return e->text;
}

std::expected<std::vector<std::string>, std::string> list(const Section& s, std::string_view key) {
    const toml::Entry* e = s.find(key);
    if (e == nullptr) {
        return std::vector<std::string>{};
    }
    if (e->kind != toml::Entry::Kind::Strings || e->list.size() > kMaxList) {
        return std::unexpected(std::format("'{}' needs a list of at most {} strings", key, kMaxList));
    }
    return e->list;
}

template <std::size_t N>
std::expected<std::array<std::uint8_t, N>, std::string> hex(const Section& s, std::string_view key) {
    auto t = text(s, key);
    if (!t) {
        return std::unexpected(t.error());
    }
    auto bytes = fromHex<N>(*t);
    if (!bytes) {
        return std::unexpected(std::format("'{}' needs {} hex digits", key, N * 2));
    }
    return *bytes;
}

bool identifier(std::string_view s) {
    if (s.empty() || s.size() > 128 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_')) {
        return false;
    }
    return s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") ==
           std::string_view::npos;
}

std::expected<Meta, std::string> read(const std::vector<Section>& sections) {
    const Section& root = sections.front();
    if (sections.size() != 1) {
        return std::unexpected("unexpected table");
    }
    const toml::Entry* f = root.find("format");
    if (f == nullptr || f->kind != toml::Entry::Kind::Integer || f->integer != kFormat) {
        return std::unexpected(std::format("needs format = {}", kFormat));
    }
    Meta m;
    auto name = text(root, "name");
    auto version = text(root, "version");
    auto kind = text(root, "kind");
    auto quartz = text(root, "quartz");
    auto archive = text(root, "archive");
    for (const auto* field : {&name, &version, &kind, &quartz, &archive}) {
        if (!*field) {
            return std::unexpected(field->error());
        }
    }
    m.name = *name;
    auto v = quartz::parseVersion(*version);
    if (!v || quartz::formatVersion(*v) != *version) {
        return std::unexpected(std::format("'{}' is not a major.minor.patch version", *version));
    }
    m.version = *v;
    auto k = parseKind(*kind);
    if (!k) {
        return std::unexpected(k.error());
    }
    m.kind = *k;
    if (auto q = quartz::parseVersion(*quartz); !q) {
        return std::unexpected(std::format("'quartz' is not a version: {}", *quartz));
    }
    m.quartz = *quartz;
    m.archive = *archive;
    const toml::Entry* fmt = root.find("qzb-format");
    if (fmt == nullptr || fmt->kind != toml::Entry::Kind::Integer || fmt->integer < 0 || fmt->integer > 65535) {
        return std::unexpected("'qzb-format' needs an integer");
    }
    m.qzbFormat = static_cast<std::uint32_t>(fmt->integer);
    auto deps = list(root, "dependencies");
    if (!deps) {
        return std::unexpected(deps.error());
    }
    for (const std::string& item : *deps) {
        auto d = parseDependency(item);
        if (!d) {
            return std::unexpected(d.error());
        }
        if (!m.dependencies.empty() && m.dependencies.back().name >= d->name) {
            return std::unexpected("dependencies must be sorted by name and unique");
        }
        m.dependencies.push_back(std::move(*d));
    }
    for (auto [key, out] : {std::pair{"capabilities", &m.capabilities}, std::pair{"interfaces", &m.interfaces},
                            std::pair{"exports", &m.exports}}) {
        auto items = list(root, key);
        if (!items) {
            return std::unexpected(items.error());
        }
        for (const std::string& item : *items) {
            if (!identifier(item) && !(std::string_view(key) == "interfaces" && item.ends_with(".qzi"))) {
                return std::unexpected(std::format("'{}' in {} is not a plain name", item, key));
            }
        }
        *out = std::move(*items);
    }
    auto sum = hex<32>(root, "checksum");
    auto key = hex<32>(root, "key");
    auto sig = hex<64>(root, "signature");
    if (!sum || !key || !sig) {
        return std::unexpected(!sum ? sum.error() : !key ? key.error() : sig.error());
    }
    m.checksum = *sum;
    m.key = *key;
    m.signature = *sig;
    return m;
}

} // namespace

std::string signedMessage(const Meta& m) {
    std::string out = std::format("qiln-artifact-v1\nname {}\nversion {}\nkind {}\nquartz {}\nqzb-format {}\n"
                                  "archive {}\nchecksum {}\n",
                                  m.name, quartz::formatVersion(m.version), kindName(m.kind), m.quartz, m.qzbFormat,
                                  m.archive, toHex(m.checksum));
    for (const Dependency& d : m.dependencies) {
        out += std::format("dependency {} {}\n", d.name, d.range);
    }
    for (const std::string& c : m.capabilities) {
        out += std::format("capability {}\n", c);
    }
    for (const std::string& i : m.interfaces) {
        out += std::format("interface {}\n", i);
    }
    for (const std::string& e : m.exports) {
        out += std::format("export {}\n", e);
    }
    return out;
}

void signMeta(Meta& m, const Seed& seed) {
    m.key = publicKeyOf(seed);
    m.signature = sign(seed, signedMessage(m));
}

bool verifyMeta(const Meta& m) { return verify(m.key, m.signature, signedMessage(m)); }

std::expected<Meta, std::string> parseMeta(std::string_view source, std::string_view name,
                                           const quartz::Version& version) {
    std::string label = std::format("{} {}", name, quartz::formatVersion(version));
    auto sections = toml::parse(source, label + ".qzmeta");
    if (!sections) {
        return std::unexpected(sections.error());
    }
    auto m = read(*sections);
    if (!m) {
        return std::unexpected(std::format("{}.qzmeta: {}", label, m.error()));
    }
    if (m->name != name || m->version != version) {
        return std::unexpected(std::format("{}.qzmeta describes {} {}", label, m->name, formatVersion(m->version)));
    }
    std::string expected = std::format("{}-{}{}", coordinatesOf(name)->artifact, formatVersion(version),
                                       archiveExtension(m->kind));
    if (m->archive != expected) {
        return std::unexpected(std::format("{}.qzmeta names the archive '{}', expected '{}'", label, m->archive,
                                           expected));
    }
    if (!verifyMeta(*m)) {
        return std::unexpected(std::format("{}.qzmeta has an invalid signature", label));
    }
    return m;
}

std::string printMeta(const Meta& m) {
    std::vector<std::string> deps;
    for (const Dependency& d : m.dependencies) {
        deps.push_back(formatDependency(d));
    }
    return std::format("format = {}\nname = \"{}\"\nversion = \"{}\"\nkind = \"{}\"\nquartz = \"{}\"\n"
                       "qzb-format = {}\narchive = \"{}\"\nchecksum = \"{}\"\ndependencies = [{}]\n"
                       "capabilities = [{}]\ninterfaces = [{}]\nexports = [{}]\nkey = \"{}\"\nsignature = \"{}\"\n",
                       kFormat, m.name, quartz::formatVersion(m.version), kindName(m.kind), m.quartz, m.qzbFormat,
                       m.archive, toHex(m.checksum), toml::quoteList(deps), toml::quoteList(m.capabilities),
                       toml::quoteList(m.interfaces), toml::quoteList(m.exports), toHex(m.key), toHex(m.signature));
}

} // namespace qiln
