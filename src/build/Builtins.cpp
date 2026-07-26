#include "build/Runner.hpp"
#include "build/TaskHost.hpp"
#include "build/Tasks.hpp"
#include "repo/Archive.hpp"
#include "repo/Publish.hpp"
#include "support/Files.hpp"

#include "ir/Serialize.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>

namespace qiln {

namespace {

constexpr std::string_view kSourceExtensions[] = {".qz", ".qzi"};

std::string artifactName(const ProjectModel& p) {
    return std::format("libs/{}-{}{}", p.name, p.version, archiveExtension(p.kind));
}

std::expected<Seed, std::string> signingSeed(const std::string& option) {
    fs::path path = option;
    if (path.empty()) {
        const char* env = std::getenv("QILN_SIGNING_KEY");
        path = env != nullptr && *env != '\0' ? fs::path(env) : qilnHome() / "signing.key";
    }
    auto text = readText(path);
    auto seed = text ? fromHex<32>(*text) : std::nullopt;
    if (!seed) {
        return std::unexpected(std::format("no signing key in {}; 'qiln keygen' creates one", path.string()));
    }
    return *seed;
}

} // namespace

std::expected<void, std::string> TaskRunner::check(const ProjectModel& p) {
    if (p.kind == ArtifactKind::BuildPlugin) {
        if (auto r = compiler_.checkBuildPlugin(p); !r) {
            return r;
        }
    }
    auto c = compiler_.compile(p, true);
    if (!c) {
        return std::unexpected(c.error());
    }
    if ((p.denyWarnings || options_.denyWarnings) && (*c)->warnings > 0) {
        return std::unexpected(std::format("{} warning(s) and warnings are denied", (*c)->warnings));
    }
    std::printf("  checked %s\n", p.packageName().c_str());
    return {};
}

std::expected<void, std::string> TaskRunner::build(const ProjectModel& p) {
    if (p.kind == ArtifactKind::BuildPlugin) {
        if (auto r = compiler_.checkBuildPlugin(p); !r) {
            return r;
        }
    }
    auto c = compiler_.compile(p, false);
    if (!c) {
        return std::unexpected(c.error());
    }
    if ((p.denyWarnings || options_.denyWarnings) && (*c)->warnings > 0) {
        return std::unexpected(std::format("{} warning(s) and warnings are denied", (*c)->warnings));
    }
    fs::path dir = ws_.projectDir(p);
    std::vector<std::uint8_t> artifact;
    if (p.kind == ArtifactKind::Library || p.kind == ArtifactKind::BuildPlugin) {
        std::vector<SourceFile> files = {{"package.toml", Compiler::packageToml(p)}};
        std::vector<std::string> dirs = p.mainDirs.empty() ? std::vector<std::string>{"src"} : p.mainDirs;
        if (p.kind == ArtifactKind::BuildPlugin) {
            dirs.emplace_back("api");
        }
        for (const std::string& i : p.interfaces) {
            files.push_back({i, readText(dir / i).value_or("")});
        }
        for (const std::string& d : dirs) {
            auto list = listFiles(dir, dir / d, kSourceExtensions);
            if (!list) {
                return std::unexpected(list.error());
            }
            for (const std::string& f : *list) {
                files.push_back({f, readText(dir / f).value_or("")});
            }
        }
        auto packed = packSources(std::move(files));
        if (!packed) {
            return std::unexpected(packed.error());
        }
        artifact = std::move(*packed);
    } else {
        artifact = (*c)->binary;
    }
    std::string name = artifactName(p);
    if (auto w = writeAtomically(dir / "build" / name, artifact); !w) {
        return std::unexpected(w.error());
    }
    artifacts_[p.path] = name;
    std::printf("  wrote %s (%zu bytes)\n", ws_.displayPath(dir / "build" / name).c_str(), artifact.size());
    return {};
}

std::expected<void, std::string> TaskRunner::test(const ProjectModel& p) {
    if (p.kind == ArtifactKind::BuildPlugin) {
        if (auto r = compiler_.checkBuildPlugin(p); !r) {
            return r;
        }
    }
    auto c = compiler_.compile(p, true);
    if (!c) {
        return std::unexpected(c.error());
    }
    auto ok = runTests(**c, p.limits, options_.testFilter);
    if (!ok) {
        return std::unexpected(ok.error());
    }
    if (!*ok) {
        return std::unexpected("some tests failed");
    }
    return {};
}

std::expected<int, std::string> TaskRunner::runEntry(const ProjectModel& p) {
    auto c = compiler_.compile(p, false);
    if (!c) {
        return std::unexpected(c.error());
    }
    std::fflush(stdout);
    return qiln::runEntry(**c, p.limits, p.entry);
}

std::expected<void, std::string> TaskRunner::publish(const ProjectModel& p) {
    if (!coordinatesOf(p.packageName())) {
        return std::unexpected(std::format("'{}' is not a package name; set p.group (like org.example) to publish",
                                           p.packageName()));
    }
    std::string target = options_.publishTo;
    fs::path base = fs::current_path();
    if (target.empty()) {
        if (p.publishRepositories.empty()) {
            return std::unexpected("nowhere to publish: add p.publishTo(Repository.local(\"...\")) or pass --to <dir>");
        }
        target = p.publishRepositories.front().url;
        base = ws_.projectDir(p);
    }
    auto repo = locate(target, base);
    if (!repo) {
        return std::unexpected(repo.error());
    }
    repo->display = repo->remote() ? repo->url : ws_.displayPath(repo->local);
    auto seed = signingSeed(options_.signingKey);
    if (!seed) {
        return std::unexpected(seed.error());
    }
    auto bytes = readLimited(ws_.projectDir(p) / "build" / artifacts_[p.path], 64u << 20);
    if (!bytes || !*bytes) {
        return std::unexpected("the artifact of " + p.path + " is missing; run build first");
    }
    Publication pub;
    pub.artifact = std::move(**bytes);
    pub.description = p.description;
    if (p.kind == ArtifactKind::Plugin || p.kind == ArtifactKind::Application) {
        quartz::ir::SecretSeed s{};
        std::copy(seed->begin(), seed->end(), s.begin());
        if (auto signedModule = quartz::ir::sign(pub.artifact, s); !signedModule) {
            return std::unexpected(signedModule.error());
        }
        pub.meta.qzbFormat = quartz::ir::kFormatVersion;
    }
    Meta& m = pub.meta;
    m.name = p.packageName();
    m.version = *quartz::parseVersion(p.version);
    m.kind = p.kind;
    m.quartz = QILN_QUARTZ_VERSION;
    m.capabilities = p.capabilities;
    m.exports = p.exports;
    for (const std::string& i : p.interfaces) {
        m.interfaces.push_back(fs::path(i).filename().string());
    }
    m.archive = std::format("{}-{}{}", coordinatesOf(m.name)->artifact, p.version, archiveExtension(p.kind));
    for (const DependencySpec& d : p.dependencies) {
        if (d.configuration != "implementation") {
            continue;
        }
        if (d.project.empty()) {
            m.dependencies.push_back({d.name, d.range});
            continue;
        }
        // A project of the workspace becomes a dependency on its published package, at its current version.
        const ProjectModel& dep = *ws_.model.findProject(d.project);
        m.dependencies.push_back({dep.packageName(), "^" + dep.version});
    }
    std::sort(m.dependencies.begin(), m.dependencies.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    for (std::size_t i = 1; i < m.dependencies.size(); ++i) {
        if (m.dependencies[i - 1].name == m.dependencies[i].name) {
            return std::unexpected(std::format("{} is listed twice as a dependency", m.dependencies[i].name));
        }
    }
    auto done = qiln::publish(*repo, pub, *seed);
    if (!done) {
        return std::unexpected(done.error());
    }
    std::printf("  %s\n", done->c_str());
    return {};
}

std::expected<void, std::string> TaskRunner::clean(const ProjectModel& p) {
    fs::path dir = ws_.projectDir(p) / "build";
    std::error_code ec;
    std::uintmax_t n = fs::remove_all(dir, ec);
    if (ec) {
        return std::unexpected(std::format("cannot delete {}: {}", ws_.displayPath(dir), ec.message()));
    }
    if (n > 0) {
        std::printf("  deleted %s\n", ws_.displayPath(dir).c_str());
    }
    return {};
}

std::expected<void, std::string> TaskRunner::pluginTask(const TaskRef& t) {
    const TaskSpec& spec = *t.spec;
    auto plugins = ws_.plugins();
    if (!plugins) {
        return std::unexpected(plugins.error());
    }
    auto plugin = std::find_if((*plugins)->begin(), (*plugins)->end(),
                               [&](const LoadedPlugin& lp) { return lp.name == spec.plugin; });
    if (plugin == (*plugins)->end()) {
        return std::unexpected(std::format("build plugin {} is not loaded", spec.plugin));
    }
    auto& program = pluginPrograms_[spec.plugin];
    if (!program) {
        auto compiled = compilePluginTasks(spec.plugin, plugin->tasks);
        if (!compiled) {
            return std::unexpected(compiled.error());
        }
        program = *compiled;
    }
    const ProjectModel& p = *t.project;
    TaskEnvironment env;
    env.project = &p;
    env.task = &spec;
    env.projectDir = ws_.projectDir(p);
    env.buildDir = env.projectDir / "build";
    env.artifact = artifacts_.contains(p.path) ? artifacts_[p.path] : artifactName(p);
    auto set = compiler_.sources(p, false);
    if (set) {
        std::string prefix = p.dir.empty() ? "" : p.dir + "/";
        for (const std::string& f : set->own) {
            env.sources.push_back(f.substr(prefix.size()));
        }
    }
    auto r = runPluginTask(program, env);
    if (!r) {
        return std::unexpected(r.error());
    }
    if (!r->empty()) {
        std::printf("  %s\n", r->c_str());
    }
    return {};
}

} // namespace qiln
