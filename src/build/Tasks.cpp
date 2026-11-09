#include "build/Tasks.hpp"

#include "build/Console.hpp"
#include "build/TaskHost.hpp"
#include "support/Files.hpp"

#include <algorithm>
#include <format>

namespace qiln {

namespace {

struct Builtin {
    std::string_view name;
    std::string_view group;
    std::string_view description;
    std::string_view dependsOn;
};

constexpr Builtin kBuiltins[] = {
    {"check", "verification", "Compiles the sources and the tests without running anything", ""},
    {"test", "verification", "Runs every @test function, each in its own sandbox", ""},
    {"build", "build", "Compiles the project and writes its artifact to build/libs", ""},
    {"run", "application", "Runs the entry function in the runner's sandbox", "build"},
    {"publish", "publishing", "Signs the artifact and publishes it to a repository", "build"},
    {"clean", "build", "Deletes the build directory", ""},
};

constexpr std::size_t kMaxStamp = 64u << 20;
constexpr std::size_t kMaxRecorded = 1u << 30;

fs::path stampPath(const fs::path& projectDir, std::string_view task) {
    return projectDir / "build" / ".qiln" / std::format("{}.stamp", task);
}

// The hash a recorded read of a plugin task has now; nullopt for an id this version does not know.
std::optional<Hash32> recorded(const fs::path& projectDir, std::string_view id) {
    std::size_t colon = id.find(':');
    std::string path(id.substr(colon == std::string_view::npos ? id.size() : colon + 1));
    if (colon == std::string_view::npos || !safeRelativePath(path)) {
        return std::nullopt;
    }
    std::string_view kind = id.substr(0, colon);
    if (kind == "exists") {
        std::error_code ec;
        return existenceHash(fs::exists(projectDir / path, ec));
    }
    if (kind != "project" && kind != "build") {
        return std::nullopt;
    }
    auto bytes = readLimited(kind == "build" ? projectDir / "build" / path : projectDir / path, kMaxRecorded);
    if (!bytes) {
        return std::nullopt;
    }
    return contentHash(*bytes);
}

} // namespace

std::string qualifiedName(const ProjectModel& p, std::string_view task) {
    return p.path == ":" ? std::format(":{}", task) : std::format("{}:{}", p.path, task);
}

std::vector<TaskRef> tasksOf(const ProjectModel& p) {
    std::vector<TaskRef> out;
    for (const Builtin& b : kBuiltins) {
        if (b.name == "run" && p.kind != ArtifactKind::Application) {
            continue;
        }
        TaskRef t{&p, std::string(b.name), std::string(b.description), std::string(b.group), {}, nullptr, true};
        if (!b.dependsOn.empty()) {
            t.dependsOn.emplace_back(b.dependsOn);
        }
        if (const TaskSpec* s = p.findTask(b.name)) {
            t.spec = s;
            t.dependsOn.insert(t.dependsOn.end(), s->dependsOn.begin(), s->dependsOn.end());
        }
        out.push_back(std::move(t));
    }
    for (const TaskSpec& s : p.tasks) {
        if (std::any_of(std::begin(kBuiltins), std::end(kBuiltins),
                        [&](const Builtin& b) { return b.name == s.name; })) {
            continue;
        }
        out.push_back(TaskRef{&p, s.name, s.description, s.group.empty() ? "other" : s.group, s.dependsOn, &s, false});
    }
    return out;
}

TaskRunner::TaskRunner(Workspace& ws, RunOptions options)
    : ws_(ws), options_(std::move(options)), compiler_(ws, options_.denyWarnings) {}

TaskRunner::~TaskRunner() = default;

std::expected<TaskRunner::Outcome, std::string> TaskRunner::execute(const TaskRef& t) {
    const ProjectModel& p = *t.project;
    if (!t.builtin) {
        if (t.spec == nullptr || t.spec->action.empty()) {
            return Outcome{};
        }
        return pluginTask(t);
    }
    auto done = [](std::expected<void, std::string> r) { return r.transform([] { return Outcome{}; }); };
    if (t.name == "check") {
        return check(p);
    }
    if (t.name == "build") {
        return build(p);
    }
    if (t.name == "test") {
        return test(p);
    }
    if (t.name == "run") {
        return runEntry(p).transform([](int code) { return Outcome{code, false}; });
    }
    if (t.name == "publish") {
        return done(publish(p));
    }
    return done(clean(p));
}

// The workspace resolves and loads lazily; doing it before the workers start keeps it on one thread.
std::expected<void, std::string> TaskRunner::prepare(const std::vector<const TaskRef*>& order) {
    bool compiles = false;
    bool plugins = false;
    for (const TaskRef* t : order) {
        compiles = compiles || (t->builtin && t->name != "clean");
        plugins = plugins || (!t->builtin && t->spec != nullptr && !t->spec->action.empty());
    }
    bool packages = std::any_of(ws_.model.projects.begin(), ws_.model.projects.end(), [](const ProjectModel& p) {
        return std::any_of(p.dependencies.begin(), p.dependencies.end(),
                           [](const DependencySpec& d) { return d.project.empty(); });
    });
    if (compiles && packages) {
        if (auto g = ws_.graph(false); !g) {
            return std::unexpected(g.error());
        }
    }
    if (plugins) {
        if (auto l = ws_.plugins(); !l) {
            return std::unexpected(l.error());
        }
    }
    return {};
}

std::expected<Hash32, std::string> TaskRunner::fingerprint(const ProjectModel& p, std::string_view task) {
    auto set = compiler_.sources(p, task != "build");
    if (!set) {
        return std::unexpected(set.error());
    }
    Hasher h;
    h.add(QILN_VERSION).add(QILN_QUARTZ_VERSION).add(task).add(Compiler::packageToml(p)).add(kindName(p.kind));
    h.add(p.hostOnly ? "host-only" : "runner");
    for (const quartz::RuleSetting& r : compiler_.rulesFor(p)) {
        h.add(r.rule).add(std::to_string(static_cast<int>(r.level)));
    }
    if (task == "test") {
        const Limits& l = p.limits;
        h.add(std::format("{} {} {} {}", l.fuel, l.memoryMiB, l.maxDepth, l.testTicks)).add(options_.testFilter);
    }
    for (const quartz::SourceInput& in : (*set)->inputs) {
        h.add(in.name).add(in.text).add(in.dependency ? "dependency" : "own");
    }
    for (const RuntimePlugin& r : (*set)->plugins) {
        h.add(r.name).add(r.checksum);
    }
    if (p.kind == ArtifactKind::BuildPlugin) {
        auto api = compiler_.apiSources(p);
        if (!api) {
            return std::unexpected(api.error());
        }
        for (const quartz::SourceInput& in : *api) {
            h.add(in.name).add(in.text);
        }
    }
    return h.finish();
}

bool TaskRunner::upToDate(const ProjectModel& p, std::string_view task, const std::pair<std::string, Hash32>& inputs) {
    if (options_.rerun) {
        return false;
    }
    fs::path dir = ws_.projectDir(p);
    auto bytes = readLimited(stampPath(dir, task), kMaxStamp);
    auto stamp = bytes && *bytes ? decodeStamp(**bytes) : std::nullopt;
    if (!stamp || stamp->inputs.empty() || stamp->inputs.front() != inputs) {
        return false;
    }
    for (std::size_t i = 1; i < stamp->inputs.size(); ++i) {
        if (recorded(dir, stamp->inputs[i].first) != stamp->inputs[i].second) {
            return false;
        }
    }
    for (const auto& [path, hash] : stamp->outputs) {
        if (!safeRelativePath(path)) {
            return false;
        }
        auto out = readLimited(dir / "build" / path, kMaxRecorded);
        if (!out || !*out || blake2b256(**out) != hash) {
            return false;
        }
    }
    return true;
}

void TaskRunner::saveStamp(const ProjectModel& p, std::string_view task, const Stamp& stamp) {
    if (auto w = writeAtomically(stampPath(ws_.projectDir(p), task), encodeStamp(stamp)); !w) {
        complain(std::format("warning: {}\n", w.error()));
    }
}

} // namespace qiln
