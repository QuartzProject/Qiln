#include "repo/Archive.hpp"
#include "support/Files.hpp"
#include "workspace/Workspace.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace qiln {

namespace {

constexpr std::string_view kSourceExtensions[] = {".qz", ".qzi"};

std::expected<std::vector<quartz::SourceInput>, std::string> readSources(const fs::path& root, const fs::path& dir,
                                                                         bool dependency) {
    auto files = listFiles(root, dir, kSourceExtensions);
    if (!files) {
        return std::unexpected(files.error());
    }
    std::vector<quartz::SourceInput> out;
    for (const std::string& rel : *files) {
        auto text = readText(root / rel);
        if (!text) {
            return std::unexpected("cannot read " + rel);
        }
        out.push_back({rel, std::move(*text), dependency});
    }
    return out;
}

// Library sources of every package `name` reaches in the plugin graph, for the plugin's task sandbox.
std::expected<void, std::string> addLibraries(Workspace& ws, const LockGraph& g, const std::vector<Dependency>& deps,
                                              std::vector<quartz::SourceInput>& out, std::set<std::string>& seen) {
    for (const Dependency& d : deps) {
        const Locked* p = g.find(d.name);
        if (p == nullptr || !seen.insert(d.name).second) {
            continue;
        }
        std::string label = std::format("{}@{}", p->name, quartz::formatVersion(p->version));
        if (p->kind != ArtifactKind::Library) {
            return std::unexpected(
                std::format("{} is a {}; build plugins can only use libraries", label, kindName(p->kind)));
        }
        auto bytes = ws.archive(*p, true);
        auto files = bytes ? unpackSources(*bytes) : std::unexpected(bytes.error());
        if (!files) {
            return std::unexpected(std::format("{}: {}", label, files.error()));
        }
        for (SourceFile& f : *files) {
            if (f.path != "package.toml") {
                out.push_back({label + "/" + f.path, std::move(f.text), true});
            }
        }
        if (auto r = addLibraries(ws, g, p->dependencies, out, seen); !r) {
            return r;
        }
    }
    return {};
}

std::string defaultName(std::string_view dir) {
    std::string leaf(dir.substr(dir.rfind('/') + 1));
    std::string out;
    for (char c : leaf) {
        char l = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == '_') {
            out += l;
        } else if (l == '-' || l == '.' || l == ' ') {
            out += '_';
        }
    }
    if (out.empty() || out[0] < 'a' || out[0] > 'z') {
        out = "project" + out;
    }
    return out.substr(0, 64);
}

} // namespace

std::string defaultProjectName(std::string_view dir) { return defaultName(dir); }

std::expected<void, std::string> Workspace::evaluateLocalPlugins() {
    if (!localPlugins_.empty() || std::none_of(model.plugins.begin(), model.plugins.end(),
                                               [](const PluginRequest& r) { return !r.dir.empty(); })) {
        return {};
    }
    for (PluginRequest& r : model.plugins) {
        if (r.dir.empty()) {
            continue;
        }
        fs::path dir = root / r.dir;
        auto text = readText(dir / "build.qz");
        if (!text) {
            return std::unexpected(
                std::format("settings.qz includes the plugin in '{}' but {}/build.qz does not exist", r.dir, r.dir));
        }
        ProjectModel pm;
        pm.dir = r.dir;
        pm.path = projectPath(r.dir);
        pm.name = defaultName(r.dir);
        if (auto e = evaluateBuild({r.dir + "/build.qz", *text}, pm, {}, {}, options.properties); !e) {
            return std::unexpected(e.error());
        }
        if (pm.kind != ArtifactKind::BuildPlugin) {
            return std::unexpected(std::format("{} is included as a plugin but its build.qz does not set "
                                               "p.kind = Kind.BuildPlugin",
                                               r.dir));
        }
        if (!coordinatesOf(pm.packageName())) {
            return std::unexpected(std::format("{}/build.qz: a plugin needs a package name like org.example.bundle "
                                               "(set p.group)",
                                               r.dir));
        }
        r.name = pm.packageName();
        for (const DependencySpec& d : pm.dependencies) {
            if (!d.project.empty()) {
                return std::unexpected(std::format("{}/build.qz: a build plugin cannot depend on a project", r.dir));
            }
            localPluginRequirements_.push_back({d.name, d.range, std::format("plugin {}", r.name)});
        }
        localPlugins_.push_back(std::move(pm));
    }
    std::set<std::string> names;
    for (const PluginRequest& r : model.plugins) {
        if (!names.insert(r.name).second) {
            return std::unexpected(std::format("build plugin {} is requested twice", r.name));
        }
    }
    return {};
}

std::expected<const std::vector<LoadedPlugin>*, std::string> Workspace::plugins() {
    if (plugins_) {
        return &*plugins_;
    }
    if (auto e = evaluateLocalPlugins(); !e) {
        return std::unexpected(e.error());
    }
    std::vector<LoadedPlugin> out;
    const LockGraph* g = nullptr;
    if (!requirements(true).empty()) {
        auto graph = this->graph(true);
        if (!graph) {
            return std::unexpected(graph.error());
        }
        g = *graph;
    }
    for (const PluginRequest& r : model.plugins) {
        LoadedPlugin lp;
        lp.name = r.name;
        std::vector<Dependency> deps;
        if (!r.dir.empty()) {
            auto pm = std::find_if(localPlugins_.begin(), localPlugins_.end(),
                                   [&](const ProjectModel& p) { return p.dir == r.dir; });
            lp.version = pm->version;
            lp.origin = r.dir;
            auto api = readSources(root, root / r.dir / "api", true);
            if (!api) {
                return std::unexpected(api.error());
            }
            lp.api = std::move(*api);
            std::vector<std::string> dirs = pm->mainDirs.empty() ? std::vector<std::string>{"src"} : pm->mainDirs;
            for (const std::string& d : dirs) {
                auto tasks = readSources(root, root / r.dir / d, false);
                if (!tasks) {
                    return std::unexpected(tasks.error());
                }
                lp.tasks.insert(lp.tasks.end(), tasks->begin(), tasks->end());
            }
            for (const DependencySpec& d : pm->dependencies) {
                deps.push_back({d.name, d.range});
            }
        } else {
            const Locked* p = g == nullptr ? nullptr : g->find(r.name);
            if (p == nullptr) {
                return std::unexpected(std::format("build plugin {} is not locked", r.name));
            }
            lp.version = quartz::formatVersion(p->version);
            lp.origin = "repository";
            std::string label = std::format("{}@{}", p->name, lp.version);
            if (p->kind != ArtifactKind::BuildPlugin) {
                return std::unexpected(std::format("{} is a {}, not a build plugin", label, kindName(p->kind)));
            }
            auto bytes = archive(*p, true);
            auto files = bytes ? unpackSources(*bytes) : std::unexpected(bytes.error());
            if (!files) {
                return std::unexpected(std::format("{}: {}", label, files.error()));
            }
            for (SourceFile& f : *files) {
                if (f.path.starts_with("api/")) {
                    lp.api.push_back({label + "/" + f.path, std::move(f.text), true});
                } else if (f.path != "package.toml") {
                    lp.tasks.push_back({label + "/" + f.path, std::move(f.text), false});
                }
            }
            deps = p->dependencies;
        }
        if (!deps.empty()) {
            std::set<std::string> seen;
            if (auto l = addLibraries(*this, *g, deps, lp.tasks, seen); !l) {
                return std::unexpected(l.error());
            }
        }
        out.push_back(std::move(lp));
    }
    plugins_ = std::move(out);
    return &*plugins_;
}

} // namespace qiln
