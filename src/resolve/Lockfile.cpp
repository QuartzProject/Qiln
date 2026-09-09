#include "resolve/Lockfile.hpp"

#include "support/Toml.hpp"

#include <format>
#include <set>

namespace qiln {

namespace {

constexpr std::int64_t kFormat = 1;

std::expected<std::string, std::string> text(const toml::Section& s, std::string_view key) {
    const toml::Entry* e = s.find(key);
    if (e == nullptr || e->kind != toml::Entry::Kind::String) {
        return std::unexpected(std::format("qiln.lock:{}: '{}' needs a string", s.line, key));
    }
    return e->text;
}

std::expected<Locked, std::string> readEntry(const toml::Section& s) {
    Locked p;
    auto name = text(s, "name");
    auto version = text(s, "version");
    auto kind = text(s, "kind");
    auto sum = text(s, "checksum");
    auto key = text(s, "key");
    for (const auto* field : {&name, &version, &kind, &sum, &key}) {
        if (!*field) {
            return std::unexpected(field->error());
        }
    }
    auto bad = [&](std::string_view what) { return std::unexpected(std::format("qiln.lock:{}: {}", s.line, what)); };
    p.name = *name;
    if (!coordinatesOf(p.name)) {
        return bad(std::format("'{}' is not a package name", p.name));
    }
    auto v = quartz::parseVersion(*version);
    if (!v || quartz::formatVersion(*v) != *version) {
        return bad(std::format("'{}' is not a major.minor.patch version", *version));
    }
    p.version = *v;
    auto k = parseKind(*kind);
    if (!k) {
        return bad(k.error());
    }
    p.kind = *k;
    auto c = fromHex<32>(*sum);
    auto pk = fromHex<32>(*key);
    if (!c || !pk) {
        return bad("checksum and key need 64 hex digits");
    }
    p.checksum = *c;
    p.key = *pk;
    if (const toml::Entry* deps = s.find("dependencies")) {
        if (deps->kind != toml::Entry::Kind::Strings || deps->list.size() > 256) {
            return bad("dependencies are a list of \"name@range\"");
        }
        for (const std::string& item : deps->list) {
            auto d = parseDependency(item);
            if (!d) {
                return bad(d.error());
            }
            p.dependencies.push_back(std::move(*d));
        }
    }
    return p;
}

void printGraph(std::string& out, std::string_view table, const LockGraph& g) {
    for (const Locked& p : g.packages) {
        std::vector<std::string> deps;
        for (const Dependency& d : p.dependencies) {
            deps.push_back(formatDependency(d));
        }
        out += std::format("\n[[{}]]\nname = \"{}\"\nversion = \"{}\"\nkind = \"{}\"\ndependencies = [{}]\n"
                           "checksum = \"{}\"\nkey = \"{}\"\n",
                           table, p.name, quartz::formatVersion(p.version), kindName(p.kind), toml::quoteList(deps),
                           toHex(p.checksum), toHex(p.key));
    }
}

} // namespace

const Locked* LockGraph::find(std::string_view name) const {
    for (const Locked& p : packages) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

std::expected<Lockfile, std::string> parseLock(std::string_view source) {
    auto sections = toml::parse(source, "qiln.lock");
    if (!sections) {
        return std::unexpected(sections.error());
    }
    const toml::Entry* f = sections->front().find("format");
    if (f == nullptr || f->kind != toml::Entry::Kind::Integer || f->integer != kFormat) {
        return std::unexpected(std::format("qiln.lock: needs format = {}; delete it to resolve again", kFormat));
    }
    Lockfile lock;
    std::set<std::pair<bool, std::string>> seen;
    for (std::size_t i = 1; i < sections->size(); ++i) {
        const toml::Section& s = (*sections)[i];
        bool plugin = s.name == "plugin";
        if (!s.array || (s.name != "package" && !plugin)) {
            return std::unexpected(std::format("qiln.lock:{}: unexpected table [{}]", s.line, s.name));
        }
        auto p = readEntry(s);
        if (!p) {
            return std::unexpected(p.error());
        }
        if (!seen.insert({plugin, p->name}).second) {
            return std::unexpected(std::format("qiln.lock:{}: {} is listed twice", s.line, p->name));
        }
        (plugin ? lock.plugins : lock.packages).packages.push_back(std::move(*p));
    }
    return lock;
}

std::string printLock(const Lockfile& lock) {
    std::string out =
        std::format("# Written by qiln: the exact packages this workspace builds with.\nformat = {}\n", kFormat);
    printGraph(out, "package", lock.packages);
    printGraph(out, "plugin", lock.plugins);
    return out;
}

std::expected<void, std::string> graphCovers(const LockGraph& graph, const std::vector<Requirement>& roots) {
    auto check = [&](const std::string& name, const std::string& range,
                     const std::string& by) -> std::expected<void, std::string> {
        const Locked* p = graph.find(name);
        if (p == nullptr) {
            return std::unexpected(std::format("{} (needed by {}) is not locked", name, by));
        }
        auto ok = quartz::satisfies(p->version, range);
        if (!ok || !*ok) {
            return std::unexpected(std::format("{} is locked at {}, outside '{}' asked by {}", name,
                                               quartz::formatVersion(p->version), range, by));
        }
        return {};
    };
    for (const Requirement& r : roots) {
        if (auto c = check(r.name, r.range, r.by); !c) {
            return c;
        }
    }
    for (const Locked& p : graph.packages) {
        for (const Dependency& d : p.dependencies) {
            if (auto c = check(d.name, d.range, std::format("{} {}", p.name, quartz::formatVersion(p.version))); !c) {
                return c;
            }
        }
    }
    return {};
}

} // namespace qiln
