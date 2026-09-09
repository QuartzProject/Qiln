#include "workspace/ConfigCache.hpp"

#include "ir/ByteIo.hpp"

#include <algorithm>

namespace qiln {

namespace {

namespace bin = quartz::ir::bin;

constexpr std::string_view kMagic = "QLNC";
constexpr std::uint64_t kVersion = 1;
constexpr std::uint64_t kMaxItems = 1u << 16;

class Out {
public:
    bin::Writer w;

    void s(std::string_view text) { w.str(text); }

    void n(std::uint64_t v) { w.u64(v); }

    void list(const std::vector<std::string>& items) {
        n(items.size());
        for (const std::string& i : items) {
            s(i);
        }
    }

    void map(const std::map<std::string, std::string>& m) {
        n(m.size());
        for (const auto& [k, v] : m) {
            s(k);
            s(v);
        }
    }
};

class In {
public:
    explicit In(std::span<const std::uint8_t> bytes) : r(bytes) {}

    bin::Reader r;

    std::string s() { return r.str(); }

    std::uint64_t n() { return r.u64(); }

    std::uint64_t count() {
        std::uint64_t c = n();
        if (c > kMaxItems || c > r.remaining()) {
            bad = true;
            return 0;
        }
        return c;
    }

    std::vector<std::string> list() {
        std::vector<std::string> out(count());
        for (std::string& i : out) {
            i = s();
        }
        return out;
    }

    std::map<std::string, std::string> map() {
        std::map<std::string, std::string> out;
        for (std::uint64_t i = 0, c = count(); i < c && !r.failed(); ++i) {
            std::string k = s();
            out[k] = s();
        }
        return out;
    }

    [[nodiscard]] bool failed() const { return bad || r.failed(); }

    bool bad = false;
};

void writeProject(Out& o, const ProjectModel& p) {
    for (const std::string* f : {&p.path, &p.dir, &p.name, &p.group, &p.version, &p.description, &p.entry}) {
        o.s(*f);
    }
    o.n(static_cast<std::uint64_t>(p.kind));
    o.n((p.denyWarnings ? 1 : 0) | (p.hostOnly ? 2 : 0));
    o.list(p.mainDirs);
    o.list(p.testDirs);
    o.list(p.interfaces);
    for (const auto* repos : {&p.repositories, &p.publishRepositories}) {
        o.n(repos->size());
        for (const RepositorySpec& r : *repos) {
            o.s(r.name);
            o.s(r.url);
        }
    }
    o.n(p.dependencies.size());
    for (const DependencySpec& d : p.dependencies) {
        for (const std::string* f : {&d.configuration, &d.project, &d.name, &d.range}) {
            o.s(*f);
        }
    }
    o.list(p.exports);
    o.list(p.capabilities);
    o.n(static_cast<std::uint64_t>(p.limits.fuel));
    o.n(static_cast<std::uint64_t>(p.limits.memoryMiB));
    o.n(static_cast<std::uint64_t>(p.limits.maxDepth));
    o.list(p.plugins);
    o.map(p.options);
    o.n(p.tasks.size());
    for (const TaskSpec& t : p.tasks) {
        for (const std::string* f : {&t.name, &t.description, &t.group, &t.plugin, &t.action}) {
            o.s(*f);
        }
        o.list(t.dependsOn);
        o.map(t.options);
    }
}

ProjectModel readProject(In& in) {
    ProjectModel p;
    for (std::string* f : {&p.path, &p.dir, &p.name, &p.group, &p.version, &p.description, &p.entry}) {
        *f = in.s();
    }
    std::uint64_t kind = in.n();
    in.bad = in.bad || kind > static_cast<std::uint64_t>(ArtifactKind::BuildPlugin);
    p.kind = static_cast<ArtifactKind>(kind);
    std::uint64_t flags = in.n();
    p.denyWarnings = (flags & 1) != 0;
    p.hostOnly = (flags & 2) != 0;
    p.mainDirs = in.list();
    p.testDirs = in.list();
    p.interfaces = in.list();
    for (auto* repos : {&p.repositories, &p.publishRepositories}) {
        for (std::uint64_t i = 0, c = in.count(); i < c && !in.failed(); ++i) {
            RepositorySpec r;
            r.name = in.s();
            r.url = in.s();
            repos->push_back(std::move(r));
        }
    }
    for (std::uint64_t i = 0, c = in.count(); i < c && !in.failed(); ++i) {
        DependencySpec d;
        for (std::string* f : {&d.configuration, &d.project, &d.name, &d.range}) {
            *f = in.s();
        }
        p.dependencies.push_back(std::move(d));
    }
    p.exports = in.list();
    p.capabilities = in.list();
    p.limits.fuel = static_cast<std::int64_t>(in.n());
    p.limits.memoryMiB = static_cast<std::int64_t>(in.n());
    p.limits.maxDepth = static_cast<std::int64_t>(in.n());
    p.plugins = in.list();
    p.options = in.map();
    for (std::uint64_t i = 0, c = in.count(); i < c && !in.failed(); ++i) {
        TaskSpec t;
        for (std::string* f : {&t.name, &t.description, &t.group, &t.plugin, &t.action}) {
            *f = in.s();
        }
        t.dependsOn = in.list();
        t.options = in.map();
        p.tasks.push_back(std::move(t));
    }
    return p;
}

} // namespace

std::vector<std::uint8_t> encodeConfiguration(const Fingerprint& inputs, const WorkspaceModel& model) {
    Out o;
    o.n(kVersion);
    o.n(inputs.size());
    for (const auto& [id, hash] : inputs) {
        o.s(id);
        o.s(std::string_view(reinterpret_cast<const char*>(hash.data()), hash.size()));
    }
    o.s(model.name);
    o.list(model.includes);
    o.n(model.pluginRepositories.size());
    for (const RepositorySpec& r : model.pluginRepositories) {
        o.s(r.name);
        o.s(r.url);
    }
    o.n(model.plugins.size());
    for (const PluginRequest& r : model.plugins) {
        o.s(r.name);
        o.s(r.range);
        o.s(r.dir);
    }
    o.n(model.projects.size());
    for (const ProjectModel& p : model.projects) {
        writeProject(o, p);
    }
    std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
    out.insert(out.end(), o.w.out.begin(), o.w.out.end());
    return out;
}

std::optional<std::pair<Fingerprint, WorkspaceModel>> decodeConfiguration(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kMagic.size() || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return std::nullopt;
    }
    In in(bytes.subspan(kMagic.size()));
    if (in.n() != kVersion) {
        return std::nullopt;
    }
    Fingerprint inputs;
    for (std::uint64_t i = 0, c = in.count(); i < c && !in.failed(); ++i) {
        std::string id = in.s();
        std::string h = in.s();
        if (h.size() != 32) {
            return std::nullopt;
        }
        Hash32 hash{};
        std::copy(h.begin(), h.end(), hash.begin());
        inputs.emplace_back(std::move(id), hash);
    }
    WorkspaceModel m;
    m.name = in.s();
    m.includes = in.list();
    for (std::uint64_t i = 0, c = in.count(); i < c && !in.failed(); ++i) {
        RepositorySpec r;
        r.name = in.s();
        r.url = in.s();
        m.pluginRepositories.push_back(std::move(r));
    }
    for (std::uint64_t i = 0, c = in.count(); i < c && !in.failed(); ++i) {
        PluginRequest r;
        r.name = in.s();
        r.range = in.s();
        r.dir = in.s();
        m.plugins.push_back(std::move(r));
    }
    for (std::uint64_t i = 0, c = in.count(); i < c && !in.failed(); ++i) {
        m.projects.push_back(readProject(in));
    }
    if (in.failed() || !in.r.atEnd()) {
        return std::nullopt;
    }
    return std::pair(std::move(inputs), std::move(m));
}

} // namespace qiln
