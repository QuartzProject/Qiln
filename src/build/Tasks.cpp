#include "build/Tasks.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <set>

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

std::string qualified(const ProjectModel& p, std::string_view task) {
    return p.path == ":" ? std::format(":{}", task) : std::format("{}:{}", p.path, task);
}

} // namespace

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

TaskRunner::TaskRunner(Workspace& ws, RunOptions options) : ws_(ws), options_(std::move(options)), compiler_(ws) {}

TaskRunner::~TaskRunner() = default;

std::expected<int, std::string> TaskRunner::run(const std::vector<std::string>& requests, const ProjectModel* current) {
    std::map<std::string, std::vector<TaskRef>> all;
    for (const ProjectModel& p : ws_.model.projects) {
        all[p.path] = tasksOf(p);
    }
    auto find = [&](const std::string& path, const std::string& name) -> const TaskRef* {
        auto it = all.find(path);
        if (it == all.end()) {
            return nullptr;
        }
        for (const TaskRef& t : it->second) {
            if (t.name == name) {
                return &t;
            }
        }
        return nullptr;
    };
    std::vector<const TaskRef*> order;
    std::set<const TaskRef*> done;
    std::vector<const TaskRef*> stack;
    auto visit = [&](auto& self, const TaskRef* t) -> std::expected<void, std::string> {
        if (done.contains(t)) {
            return {};
        }
        if (std::find(stack.begin(), stack.end(), t) != stack.end()) {
            std::string cycle;
            for (const TaskRef* s : stack) {
                cycle += qualified(*s->project, s->name) + " -> ";
            }
            return std::unexpected("tasks depend on each other: " + cycle + qualified(*t->project, t->name));
        }
        stack.push_back(t);
        for (const std::string& dep : t->dependsOn) {
            std::string path = t->project->path;
            std::string name = dep;
            if (dep.starts_with(':')) {
                std::size_t last = dep.rfind(':');
                path = last == 0 ? ":" : dep.substr(0, last);
                name = dep.substr(last + 1);
            }
            const TaskRef* target = find(path, name);
            if (target == nullptr) {
                return std::unexpected(
                    std::format("{} depends on {}, which does not exist", qualified(*t->project, t->name), dep));
            }
            if (auto r = self(self, target); !r) {
                return r;
            }
        }
        stack.pop_back();
        done.insert(t);
        order.push_back(t);
        return {};
    };
    for (const std::string& request : requests) {
        std::vector<const TaskRef*> roots;
        if (request.starts_with(':')) {
            std::size_t last = request.rfind(':');
            std::string path = last == 0 ? ":" : request.substr(0, last);
            if (const TaskRef* t = find(path, request.substr(last + 1))) {
                roots.push_back(t);
            }
        } else {
            for (const ProjectModel& p : ws_.model.projects) {
                if (current == nullptr || current == &p) {
                    if (const TaskRef* t = find(p.path, request)) {
                        roots.push_back(t);
                    }
                }
            }
        }
        if (roots.empty()) {
            return std::unexpected(std::format("no task '{}' here; 'qiln tasks' lists them", request));
        }
        for (const TaskRef* t : roots) {
            if (auto r = visit(visit, t); !r) {
                return std::unexpected(r.error());
            }
        }
    }
    int code = 0;
    for (const TaskRef* t : order) {
        std::printf("> %s\n", qualified(*t->project, t->name).c_str());
        std::fflush(stdout);
        auto r = execute(*t);
        if (!r) {
            return std::unexpected(std::format("{} failed: {}", qualified(*t->project, t->name), r.error()));
        }
        code = code != 0 ? code : *r;
    }
    return code;
}

std::expected<int, std::string> TaskRunner::execute(const TaskRef& t) {
    const ProjectModel& p = *t.project;
    if (!t.builtin) {
        if (t.spec == nullptr || t.spec->action.empty()) {
            return 0;
        }
        return pluginTask(t).transform([] { return 0; });
    }
    auto done = [](std::expected<void, std::string> r) { return r.transform([] { return 0; }); };
    if (t.name == "check") {
        return done(check(p));
    }
    if (t.name == "build") {
        return done(build(p));
    }
    if (t.name == "test") {
        return done(test(p));
    }
    if (t.name == "run") {
        return runEntry(p);
    }
    if (t.name == "publish") {
        return done(publish(p));
    }
    return done(clean(p));
}

} // namespace qiln
