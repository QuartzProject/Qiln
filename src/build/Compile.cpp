#include "build/Compile.hpp"

#include "QilnApi.hpp"
#include "build/Console.hpp"
#include "repo/Archive.hpp"
#include "support/Files.hpp"

#include <quartz/Package.hpp>

#include <cstdio>
#include <format>
#include <map>
#include <set>

namespace qiln {

namespace {

constexpr std::string_view kSourceExtensions[] = {".qz", ".qzi"};

std::expected<std::vector<std::string>, std::string>
filesOf(const Workspace& ws, const ProjectModel& p, const std::vector<std::string>& dirs, std::string_view fallback) {
    std::vector<std::string> out;
    std::vector<std::string> list = dirs.empty() ? std::vector<std::string>{std::string(fallback)} : dirs;
    for (const std::string& d : list) {
        auto files = listFiles(ws.root, ws.projectDir(p) / d, kSourceExtensions);
        if (!files) {
            return std::unexpected(files.error());
        }
        out.insert(out.end(), files->begin(), files->end());
    }
    return out;
}

std::expected<void, std::string> read(const Workspace& ws, const std::vector<std::string>& files, bool dependency,
                                      std::vector<quartz::SourceInput>& out) {
    for (const std::string& f : files) {
        auto text = readText(ws.root / f);
        if (!text) {
            return std::unexpected("cannot read " + f);
        }
        out.push_back({f, std::move(*text), dependency});
    }
    return {};
}

// A failed compilation whose only errors are raised warnings says which rules did it.
std::string failure(const quartz::CompileOutput& out, std::string_view what) {
    std::map<std::string, std::size_t> raised;
    std::size_t errors = 0;
    for (const quartz::CompileMessage& m : out.messages) {
        if (m.severity == quartz::Severity::Error) {
            ++errors;
            if (!m.rule.empty()) {
                ++raised[m.rule];
            }
        }
    }
    std::size_t count = 0;
    std::string rules;
    for (const auto& [rule, n] : raised) {
        count += n;
        rules += std::format("{}{} {}", rules.empty() ? "" : ", ", rule, n);
    }
    if (count == 0 || count != errors) {
        return std::format("{} does not compile", what);
    }
    return std::format("{} does not compile: {} warning(s) are errors ({})", what, count, rules);
}

} // namespace

std::vector<quartz::RuleSetting> Compiler::rulesFor(const ProjectModel& p) const {
    std::vector<quartz::RuleSetting> out;
    if (p.denyWarnings || denyWarnings_) {
        out.push_back({"*", quartz::RuleLevel::Error});
    }
    for (const RuleSpec& r : p.rules) {
        out.push_back({r.rule, static_cast<quartz::RuleLevel>(r.level)});
    }
    return out;
}

std::string Compiler::packageToml(const ProjectModel& p) {
    std::string out = std::format("name = \"{}\"\nversion = \"{}\"\nkind = \"{}\"\n", p.packageName(), p.version,
                                  p.kind == ArtifactKind::Plugin ? "plugin" : "library");
    auto list = [](const std::vector<std::string>& items) {
        std::string s;
        for (const std::string& i : items) {
            s += std::format("{}\"{}\"", s.empty() ? "" : ", ", i);
        }
        return s;
    };
    if (!p.exports.empty()) {
        out += std::format("exports = [{}]\n", list(p.exports));
    }
    if (!p.capabilities.empty()) {
        out += std::format("capabilities = [{}]\n", list(p.capabilities));
    }
    std::string deps;
    for (const DependencySpec& d : p.dependencies) {
        if (d.project.empty() && d.configuration == "implementation") {
            deps += std::format("\"{}\" = \"{}\"\n", d.name, d.range);
        }
    }
    if (!deps.empty()) {
        out += "\n[dependencies]\n" + deps;
    }
    return out;
}

// Libraries are compiled in; host plugins are loaded, checked against their locked key and linked (D94).
std::expected<void, std::string> Compiler::addPackages(const std::vector<std::string>& roots, SourceSet& out) {
    if (roots.empty()) {
        return {};
    }
    std::lock_guard lock(packages_);
    auto graph = ws_.graph(false);
    if (!graph) {
        return std::unexpected(graph.error());
    }
    const LockGraph& g = **graph;
    std::set<std::string> inside; // compiled in: libraries, and the interfaces of plugins reached through them
    std::set<std::string> linked; // host plugins to load, also those only other plugins use
    std::set<std::pair<std::string, bool>> seen;
    std::vector<std::pair<std::string, bool>> work;
    for (const std::string& r : roots) {
        work.emplace_back(r, true);
    }
    while (!work.empty()) {
        auto [name, compiledIn] = work.back();
        work.pop_back();
        if (!seen.insert({name, compiledIn}).second) {
            continue;
        }
        const Locked* p = g.find(name);
        if (p == nullptr) {
            return std::unexpected(std::format("{} is not locked", name));
        }
        if (p->kind == ArtifactKind::Application || p->kind == ArtifactKind::BuildPlugin) {
            return std::unexpected(std::format("{} is a {} and cannot be a dependency", name, kindName(p->kind)));
        }
        bool isPlugin = p->kind == ArtifactKind::Plugin;
        if (compiledIn) {
            inside.insert(name);
        }
        if (isPlugin) {
            linked.insert(name);
        }
        for (const Dependency& d : p->dependencies) {
            work.emplace_back(d.name, compiledIn && !isPlugin);
        }
    }
    for (const Locked& p : g.packages) {
        bool library = p.kind == ArtifactKind::Library;
        if (library ? !inside.contains(p.name) : !linked.contains(p.name)) {
            continue;
        }
        std::string label = std::format("{}@{}", p.name, quartz::formatVersion(p.version));
        if (library) {
            auto cached = libraries_.find(p.name);
            if (cached == libraries_.end()) {
                auto bytes = ws_.archive(p, false);
                auto files = bytes ? unpackSources(*bytes) : std::unexpected(bytes.error());
                if (!files) {
                    return std::unexpected(std::format("{}: {}", label, files.error()));
                }
                std::vector<quartz::SourceInput> inputs;
                for (SourceFile& f : *files) {
                    if (f.path == "package.toml") {
                        auto m = quartz::parseManifest(f.text);
                        if (!m || m->name != p.name || m->version != quartz::formatVersion(p.version)) {
                            return std::unexpected(std::format("{}: its package.toml does not match the lock", label));
                        }
                    } else {
                        inputs.push_back({label + "/" + f.path, std::move(f.text), true});
                    }
                }
                cached = libraries_.emplace(p.name, std::move(inputs)).first;
            }
            out.inputs.insert(out.inputs.end(), cached->second.begin(), cached->second.end());
            continue;
        }
        auto cached = runtimePlugins_.find(p.name);
        if (cached == runtimePlugins_.end()) {
            auto bytes = ws_.archive(p, false);
            if (!bytes) {
                return std::unexpected(bytes.error());
            }
            quartz::TrustPolicy policy;
            policy.keys.emplace_back(p.name, p.key);
            auto loaded = quartz::loadPackage(*bytes, policy);
            if (!loaded) {
                return std::unexpected(std::format("{}: {}", label, loaded.error()));
            }
            cached =
                runtimePlugins_.emplace(p.name, RuntimePlugin{p.name, std::move(loaded->program), toHex(p.checksum)})
                    .first;
        }
        if (inside.contains(p.name)) {
            auto m = quartz::parseManifest(cached->second.program->module->manifest);
            auto iface = m ? quartz::interfaceOf(*cached->second.program, *m) : std::unexpected(m.error());
            if (!iface) {
                return std::unexpected(std::format("{}: {}", label, iface.error()));
            }
            out.inputs.push_back({label + ".qzi", std::move(*iface), true});
        }
        out.plugins.push_back(cached->second);
    }
    return {};
}

template <class T>
Compiler::Once<T>& Compiler::slot(std::map<Key, std::unique_ptr<Once<T>>>& map, const Key& key) {
    std::lock_guard lock(mutex_);
    auto& entry = map[key];
    if (!entry) {
        entry = std::make_unique<Once<T>>();
    }
    return *entry;
}

std::expected<const SourceSet*, std::string> Compiler::sources(const ProjectModel& p, bool tests) {
    Once<SourceSet>& s = slot(sources_, {p.path, tests});
    std::call_once(s.flag, [&] { s.value = readSources(p, tests); });
    return s.value.transform([](const SourceSet& set) { return &set; });
}

std::expected<SourceSet, std::string> Compiler::readSources(const ProjectModel& p, bool tests) {
    SourceSet out;
    bool buildPlugin = p.kind == ArtifactKind::BuildPlugin;
    if (buildPlugin) {
        out.inputs.push_back({"<qiln>/task.qzi", std::string(api::kTaskQzi), true});
    } else if (!p.hostOnly) {
        out.inputs.push_back({std::string(kRunnerInterfaceName), std::string(kRunnerInterface), true});
    }
    for (const std::string& i : p.interfaces) {
        std::string rel = p.dir.empty() ? i : p.dir + "/" + i;
        if (auto r = read(ws_, {rel}, false, out.inputs); !r) {
            return std::unexpected(std::format("project {}: host interface {}", p.path, r.error()));
        }
    }
    auto own = filesOf(ws_, p, p.mainDirs, "src");
    if (!own) {
        return std::unexpected(own.error());
    }
    if (tests) {
        auto test = filesOf(ws_, p, p.testDirs, "test");
        if (!test) {
            return std::unexpected(test.error());
        }
        own->insert(own->end(), test->begin(), test->end());
    }
    if (own->empty()) {
        return std::unexpected(std::format("project {} has no sources in {}/{}", p.path, p.dir.empty() ? "." : p.dir,
                                           p.mainDirs.empty() ? "src" : p.mainDirs.front()));
    }
    if (auto r = read(ws_, *own, false, out.inputs); !r) {
        return std::unexpected(r.error());
    }
    out.own = std::move(*own);
    std::vector<std::string> packages;
    std::set<std::string> visited;
    auto collect = [&](auto& self, const ProjectModel& q, bool withTests) -> std::expected<void, std::string> {
        for (const DependencySpec& d : q.dependencies) {
            if (d.configuration == "test" && !withTests) {
                continue;
            }
            if (d.project.empty()) {
                packages.push_back(d.name);
                continue;
            }
            const ProjectModel& dep = *ws_.model.findProject(d.project);
            if (!visited.insert(dep.path).second) {
                continue;
            }
            auto files = filesOf(ws_, dep, dep.mainDirs, "src");
            if (!files) {
                return std::unexpected(files.error());
            }
            if (auto r = read(ws_, *files, true, out.inputs); !r) {
                return r;
            }
            if (auto r = self(self, dep, false); !r) {
                return r;
            }
        }
        return {};
    };
    if (auto r = collect(collect, p, tests); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = addPackages(packages, out); !r) {
        return std::unexpected(r.error());
    }
    return out;
}

std::expected<const Compiled*, std::string> Compiler::compile(const ProjectModel& p, bool tests) {
    Once<Compiled>& s = slot(compiled_, {p.path, tests});
    std::call_once(s.flag, [&] { s.value = compileNow(p, tests); });
    return s.value.transform([](const Compiled& c) { return &c; });
}

std::expected<Compiled, std::string> Compiler::compileNow(const ProjectModel& p, bool tests) {
    auto set = sources(p, tests);
    if (!set) {
        return std::unexpected(set.error());
    }
    quartz::CompileOptions opts;
    opts.rules = rulesFor(p);
    bool module = !tests && (p.kind == ArtifactKind::Application || p.kind == ArtifactKind::Plugin);
    opts.emitBinary = module;
    if (module) {
        opts.manifest = packageToml(p);
    }
    quartz::CompileOutput out = quartz::compile((*set)->inputs, opts);
    complain(out.diagnostics);
    if (!out.program) {
        return std::unexpected(failure(out, std::format("project {}", p.path)));
    }
    return Compiled{out.program, std::move(out.binary), out.warnings, (*set)->plugins};
}

std::expected<std::vector<quartz::SourceInput>, std::string> Compiler::apiSources(const ProjectModel& p) const {
    std::vector<quartz::SourceInput> out;
    auto files = filesOf(ws_, p, {"api"}, "api");
    if (!files) {
        return std::unexpected(files.error());
    }
    if (auto r = read(ws_, *files, false, out); !r) {
        return std::unexpected(r.error());
    }
    return out;
}

std::expected<void, std::string> Compiler::checkBuildPlugin(const ProjectModel& p) {
    std::vector<quartz::SourceInput> inputs = {{"<qiln>/qiln.qzi", std::string(api::kQilnQzi), true},
                                               {"<qiln>/qiln.qz", std::string(api::kQilnQz), true}};
    auto api = apiSources(p);
    if (!api) {
        return std::unexpected(api.error());
    }
    inputs.insert(inputs.end(), api->begin(), api->end());
    quartz::CompileOptions opts;
    opts.rules = rulesFor(p);
    quartz::CompileOutput out = quartz::compile(inputs, opts);
    complain(out.diagnostics);
    if (!out.program) {
        return std::unexpected(failure(out, std::format("the API of build plugin {}", p.path)));
    }
    return {};
}

} // namespace qiln
