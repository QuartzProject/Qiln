#include "workspace/Workspace.hpp"

#include "QilnApi.hpp"
#include "support/Files.hpp"
#include "workspace/ConfigCache.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace qiln {

namespace {

constexpr std::string_view kCacheFile = ".qiln/configuration.bin";
constexpr std::string_view kSourceExtensions[] = {".qz", ".qzi"};

std::expected<fs::path, std::string> findRoot(const fs::path& start, bool& hasSettings) {
    std::error_code ec;
    fs::path dir = fs::weakly_canonical(fs::absolute(start, ec), ec);
    std::optional<fs::path> nearestBuild;
    for (fs::path d = dir;; d = d.parent_path()) {
        if (fs::is_regular_file(d / "settings.qz", ec)) {
            hasSettings = true;
            return d;
        }
        if (!nearestBuild && fs::is_regular_file(d / "build.qz", ec)) {
            nearestBuild = d;
        }
        if (d == d.parent_path()) {
            break;
        }
    }
    if (nearestBuild) {
        return *nearestBuild;
    }
    return std::unexpected(
        std::format("no build.qz or settings.qz in {} or above it; 'qiln init' creates a project", dir.string()));
}

Hash32 hashFile(const fs::path& path) {
    Hasher h;
    if (auto text = readText(path)) {
        h.add("file").add(*text);
    } else {
        h.add("absent");
    }
    return h.finish();
}

Hash32 hashDir(const fs::path& root, const std::string& dir) {
    Hasher h;
    auto files = listFiles(root, root / dir, kSourceExtensions);
    for (const std::string& f : files ? *files : std::vector<std::string>{}) {
        h.add(f).add(readText(root / f).value_or(""));
    }
    return h.finish();
}

// Every input a configuration depends on; reading them again tells whether the cached one is still right.
Hash32 inputHash(const Workspace& ws, std::string_view id) {
    if (id.starts_with("file:")) {
        return hashFile(ws.root / std::string(id.substr(5)));
    }
    if (id.starts_with("dir:")) {
        return hashDir(ws.root, std::string(id.substr(4)));
    }
    Hasher h;
    if (id == "qiln") {
        h.add(QILN_VERSION).add(QILN_QUARTZ_VERSION).add(api::kQilnQzi).add(api::kQilnQz);
    } else if (id == "properties") {
        for (const auto& [k, v] : ws.options.properties) {
            h.add(k).add(v);
        }
    } else if (id == "root") {
        h.add(ws.root.filename().string());
    } else if (id == "lock:plugins") {
        if (auto text = readText(ws.root / "qiln.lock")) {
            auto lock = parseLock(*text);
            for (const Locked& p : lock ? lock->plugins.packages : std::vector<Locked>{}) {
                h.add(p.name).add(toHex(p.checksum));
            }
        }
    }
    return h.finish();
}

Fingerprint fingerprintOf(const Workspace& ws) {
    std::vector<std::string> ids = {"qiln", "properties", "root", "lock:plugins", "file:settings.qz", "file:build.qz"};
    for (const std::string& dir : ws.model.includes) {
        ids.push_back(std::format("file:{}/build.qz", dir));
    }
    for (const PluginRequest& r : ws.model.plugins) {
        if (!r.dir.empty()) {
            ids.push_back(std::format("file:{}/build.qz", r.dir));
            ids.push_back(std::format("dir:{}/api", r.dir));
        }
    }
    Fingerprint out;
    for (std::string& id : ids) {
        Hash32 h = inputHash(ws, id);
        out.emplace_back(std::move(id), h);
    }
    return out;
}

bool cacheCurrent(Workspace& ws) {
    auto bytes = readLimited(ws.root / kCacheFile, 64u << 20);
    if (!bytes || !*bytes) {
        return false;
    }
    auto decoded = decodeConfiguration(**bytes);
    if (!decoded) {
        return false;
    }
    for (const auto& [id, hash] : decoded->first) {
        if (inputHash(ws, id) != hash) {
            return false;
        }
    }
    ws.model = std::move(decoded->second);
    return true;
}

std::expected<void, std::string> checkProjects(const WorkspaceModel& m) {
    std::set<std::string> packages;
    for (const ProjectModel& p : m.projects) {
        if (!p.group.empty() && !packages.insert(p.packageName()).second) {
            return std::unexpected(std::format("two projects are called {}", p.packageName()));
        }
        for (const DependencySpec& d : p.dependencies) {
            if (d.project.empty()) {
                continue;
            }
            const ProjectModel* target = m.findProject(d.project);
            if (target == nullptr) {
                return std::unexpected(
                    std::format("project {} depends on {}, which settings.qz does not include", p.path, d.project));
            }
            if (target->kind != ArtifactKind::Library || target == &p) {
                return std::unexpected(
                    std::format("project {} depends on {}, which is not a library", p.path, d.project));
            }
        }
    }
    // A cycle between projects would compile each one into the other.
    std::map<std::string, int> state;
    auto visit = [&](auto& self, const ProjectModel& p) -> std::expected<void, std::string> {
        int& s = state[p.path];
        if (s == 2) {
            return {};
        }
        if (s == 1) {
            return std::unexpected(std::format("projects depend on each other in a cycle through {}", p.path));
        }
        s = 1;
        for (const DependencySpec& d : p.dependencies) {
            if (!d.project.empty()) {
                if (auto r = self(self, *m.findProject(d.project)); !r) {
                    return r;
                }
            }
        }
        state[p.path] = 2;
        return {};
    };
    for (const ProjectModel& p : m.projects) {
        if (auto r = visit(visit, p); !r) {
            return r;
        }
    }
    return {};
}

std::expected<void, std::string> evaluate(Workspace& ws) {
    WorkspaceModel& m = ws.model;
    m.name = defaultProjectName(ws.root.filename().string());
    if (ws.hasSettings) {
        auto text = readText(ws.root / "settings.qz");
        if (!text) {
            return std::unexpected("cannot read settings.qz");
        }
        if (auto e = evaluateSettings({"settings.qz", *text}, m, ws.options.properties); !e) {
            return std::unexpected(e.error());
        }
    }
    auto plugins =
        m.plugins.empty() ? std::expected<const std::vector<LoadedPlugin>*, std::string>(nullptr) : ws.plugins();
    if (!plugins) {
        return std::unexpected(plugins.error());
    }
    std::vector<quartz::SourceInput> apis;
    std::vector<std::string> requested;
    for (const LoadedPlugin& p : *plugins ? **plugins : std::vector<LoadedPlugin>{}) {
        apis.insert(apis.end(), p.api.begin(), p.api.end());
        requested.push_back(p.name);
    }
    std::vector<std::string> dirs;
    std::error_code ec;
    if (fs::is_regular_file(ws.root / "build.qz", ec)) {
        dirs.emplace_back();
    }
    dirs.insert(dirs.end(), m.includes.begin(), m.includes.end());
    for (const std::string& dir : dirs) {
        std::string file = dir.empty() ? "build.qz" : dir + "/build.qz";
        auto text = readText(ws.root / file);
        if (!text) {
            return std::unexpected(std::format("settings.qz includes '{}' but {} does not exist", dir, file));
        }
        ProjectModel p;
        p.dir = dir;
        p.path = projectPath(dir);
        p.name = dir.empty() ? m.name : defaultProjectName(dir);
        if (auto e = evaluateBuild({file, *text}, p, apis, requested, ws.options.properties); !e) {
            return std::unexpected(e.error());
        }
        m.projects.push_back(std::move(p));
    }
    return checkProjects(m);
}

} // namespace

std::expected<std::unique_ptr<Workspace>, std::string> loadWorkspace(const fs::path& start, const Options& options) {
    auto ws = std::make_unique<Workspace>();
    ws->options = options;
    auto root = findRoot(start, ws->hasSettings);
    if (!root) {
        return std::unexpected(root.error());
    }
    ws->root = *root;
    if (!options.noCache && cacheCurrent(*ws)) {
        ws->fromCache = true;
        return ws;
    }
    if (auto e = evaluate(*ws); !e) {
        return std::unexpected(e.error());
    }
    std::vector<std::uint8_t> bytes = encodeConfiguration(fingerprintOf(*ws), ws->model);
    if (auto w = writeAtomically(ws->root / kCacheFile, bytes); !w && !options.quiet) {
        std::fprintf(stderr, "warning: %s\n", w.error().c_str());
    }
    return ws;
}

const ProjectModel* currentProject(const Workspace& ws, const fs::path& dir) {
    std::error_code ec;
    fs::path rel = fs::relative(fs::weakly_canonical(fs::absolute(dir, ec), ec), ws.root, ec);
    std::string r = ec ? std::string() : rel.generic_string();
    if (r == ".") {
        r.clear();
    }
    const ProjectModel* best = nullptr;
    for (const ProjectModel& p : ws.model.projects) {
        bool inside = p.dir.empty() || r == p.dir || r.starts_with(p.dir + "/");
        if (inside && (best == nullptr || p.dir.size() > best->dir.size())) {
            best = &p;
        }
    }
    if (best != nullptr && best->dir.empty() && ws.model.projects.size() > 1 && r.empty()) {
        return nullptr;
    }
    return best;
}

} // namespace qiln
