#include "repo/Repositories.hpp"
#include "resolve/Resolver.hpp"
#include "support/Files.hpp"
#include "workspace/Workspace.hpp"

#include <cstdio>
#include <format>
#include <set>

namespace qiln {

Workspace::Workspace() = default;

Workspace::~Workspace() = default;

std::string Workspace::displayPath(const fs::path& path) const {
    std::error_code ec;
    fs::path rel = fs::relative(path, root, ec);
    return ec || rel.empty() ? path.string() : rel.generic_string();
}

std::vector<Requirement> Workspace::requirements(bool pluginGraph) const {
    std::vector<Requirement> out;
    if (pluginGraph) {
        for (const PluginRequest& r : model.plugins) {
            if (!r.range.empty()) {
                out.push_back({r.name, r.range, "settings.qz"});
            }
        }
        out.insert(out.end(), localPluginRequirements_.begin(), localPluginRequirements_.end());
        return out;
    }
    for (const ProjectModel& p : model.projects) {
        for (const DependencySpec& d : p.dependencies) {
            if (d.project.empty()) {
                out.push_back({d.name, d.range, std::format("project {}", p.path)});
            }
        }
    }
    return out;
}

std::expected<Lockfile*, std::string> Workspace::loadLock() {
    if (lock_) {
        return &*lock_;
    }
    lock_.emplace();
    if (auto text = readText(root / "qiln.lock")) {
        auto parsed = parseLock(*text);
        if (!parsed) {
            return std::unexpected(parsed.error() + " (delete qiln.lock to resolve again)");
        }
        *lock_ = std::move(*parsed);
    }
    return &*lock_;
}

std::expected<Repositories*, std::string> Workspace::repositories(bool pluginGraph) {
    std::unique_ptr<Repositories>& slot = repositories_[pluginGraph ? 1 : 0];
    if (slot) {
        return slot.get();
    }
    std::vector<Location> locations;
    std::set<std::string> seen;
    auto add = [&](const RepositorySpec& r, const fs::path& base) -> std::expected<void, std::string> {
        auto loc = locate(r.url, base);
        if (!loc) {
            return std::unexpected(loc.error());
        }
        std::string id = loc->remote() ? loc->url : fs::weakly_canonical(loc->local).string();
        if (seen.insert(id).second) {
            loc->display = loc->remote() ? loc->url : displayPath(loc->local);
            locations.push_back(std::move(*loc));
        }
        return {};
    };
    if (pluginGraph) {
        for (const RepositorySpec& r : model.pluginRepositories) {
            if (auto a = add(r, root); !a) {
                return std::unexpected("settings.qz: " + a.error());
            }
        }
        for (const ProjectModel& p : localPlugins_) {
            for (const RepositorySpec& r : p.repositories) {
                if (auto a = add(r, root / p.dir); !a) {
                    return std::unexpected(std::format("{}/build.qz: {}", p.dir, a.error()));
                }
            }
        }
    } else {
        for (const ProjectModel& p : model.projects) {
            for (const RepositorySpec& r : p.repositories) {
                if (auto a = add(r, projectDir(p)); !a) {
                    return std::unexpected(std::format("project {}: {}", p.path, a.error()));
                }
            }
        }
    }
    slot = std::make_unique<Repositories>(std::move(locations));
    return slot.get();
}

namespace {

// Drops what the requirements no longer reach, so a removed dependency leaves the lock too.
LockGraph reachable(const LockGraph& g, const std::vector<Requirement>& roots) {
    std::set<std::string> keep;
    std::vector<std::string> work;
    for (const Requirement& r : roots) {
        work.push_back(r.name);
    }
    while (!work.empty()) {
        std::string name = std::move(work.back());
        work.pop_back();
        const Locked* p = g.find(name);
        if (p == nullptr || !keep.insert(name).second) {
            continue;
        }
        for (const Dependency& d : p->dependencies) {
            work.push_back(d.name);
        }
    }
    LockGraph out;
    for (const Locked& p : g.packages) {
        if (keep.contains(p.name)) {
            out.packages.push_back(p);
        }
    }
    return out;
}

bool sameGraph(const LockGraph& a, const LockGraph& b) {
    if (a.packages.size() != b.packages.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.packages.size(); ++i) {
        const Locked& x = a.packages[i];
        const Locked& y = b.packages[i];
        if (x.name != y.name || x.version != y.version || x.checksum != y.checksum) {
            return false;
        }
    }
    return true;
}

} // namespace

std::expected<const LockGraph*, std::string> Workspace::graph(bool pluginGraph) {
    auto lock = loadLock();
    if (!lock) {
        return std::unexpected(lock.error());
    }
    LockGraph& g = pluginGraph ? (*lock)->plugins : (*lock)->packages;
    if (checked_[pluginGraph ? 1 : 0]) {
        return &g;
    }
    std::vector<Requirement> roots = requirements(pluginGraph);
    LockGraph next;
    auto covered = graphCovers(g, roots);
    if (covered && !options.update) {
        next = reachable(g, roots);
    } else {
        if (options.locked) {
            return std::unexpected(std::format("qiln.lock is out of date ({}); run without --locked to update it",
                                               covered ? "--update was given" : covered.error()));
        }
        auto repos = repositories(pluginGraph);
        if (!repos) {
            return std::unexpected(repos.error());
        }
        if ((*repos)->empty()) {
            return std::unexpected(std::format("{} need packages but no repository is declared ({})",
                                               pluginGraph ? "build plugins" : "the projects",
                                               pluginGraph ? "s.pluginRepository(...)" : "p.repository(...)"));
        }
        Repositories* r = *repos;
        PackageSource source{[r](const std::string& name) { return r->versions(name); },
                             [r](const std::string& name, const quartz::Version& v) { return r->meta(name, v); }};
        ResolveOptions ro{&g, options.update, options.allowKeyChange};
        auto resolved = resolve(roots, source, ro);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        next = std::move(*resolved);
        for (const Locked& p : next.packages) {
            const Locked* old = g.find(p.name);
            if (!options.quiet && (old == nullptr || old->version != p.version)) {
                std::fprintf(stderr, "locked %s %s (%s)%s\n", p.name.c_str(),
                             quartz::formatVersion(p.version).c_str(), std::string(kindName(p.kind)).c_str(),
                             old == nullptr ? "" : std::format(", was {}", formatVersion(old->version)).c_str());
            }
        }
    }
    bool changed = !sameGraph(g, next);
    g = std::move(next);
    checked_[pluginGraph ? 1 : 0] = true;
    if (changed) {
        if (auto w = writeAtomically(root / "qiln.lock", printLock(**lock)); !w) {
            return std::unexpected(w.error());
        }
    }
    return &g;
}

std::expected<std::vector<std::uint8_t>, std::string> Workspace::archive(const Locked& p, bool pluginGraph) {
    auto repos = repositories(pluginGraph);
    if (!repos) {
        return std::unexpected(repos.error());
    }
    return (*repos)->artifact(p);
}

} // namespace qiln
