#include "cli/Commands.hpp"
#include "repo/Repositories.hpp"
#include "script/Edit.hpp"
#include "script/Host.hpp"
#include "support/Files.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <functional>

namespace qiln::cli {

namespace {

int fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    return 1;
}

// `org.example.text@^1.2`, `org.example.text:^1.2`, `org.example.text` or `:core`; the range is empty when not given.
std::expected<std::pair<std::string, std::string>, std::string> coordinate(std::string_view text) {
    if (text.starts_with(':')) {
        if (text == ":" || !host::validTaskRef(std::string(text) + ":x")) {
            return std::unexpected(std::format("'{}' is not a project path like :core", text));
        }
        return std::pair(std::string(text), std::string());
    }
    std::size_t at = text.find_first_of("@:");
    std::string name(text.substr(0, at));
    if (!coordinatesOf(name)) {
        return std::unexpected(std::format("'{}' is not a package name like org.example.text", name));
    }
    if (at == std::string_view::npos) {
        return std::pair(std::move(name), std::string());
    }
    auto n = host::parseNotation(name + ":" + std::string(text.substr(at + 1)));
    if (!n) {
        return std::unexpected(n.error());
    }
    return std::move(*n);
}

bool declares(const ProjectModel& p, const std::string& subject, const std::string& range, bool test) {
    return std::any_of(p.dependencies.begin(), p.dependencies.end(), [&](const DependencySpec& d) {
        bool same = subject.starts_with(':') ? d.project == subject : d.name == subject && d.range == range;
        return same && (d.configuration == "test") == test;
    });
}

bool names(const ProjectModel& p, const std::string& subject) {
    return std::any_of(p.dependencies.begin(), p.dependencies.end(),
                       [&](const DependencySpec& d) { return d.project == subject || d.name == subject; });
}

struct Target {
    std::unique_ptr<Workspace> ws;
    const ProjectModel* project = nullptr;
    fs::path script;
    std::string text;
};

std::expected<Target, std::string> target(const Args& a) {
    auto ws = loadWorkspace(a.dir, a.options);
    if (!ws) {
        return std::unexpected(ws.error());
    }
    const ProjectModel* p = currentProject(**ws, a.dir);
    if (p == nullptr) {
        return std::unexpected("this is the root of a workspace of several projects; run it in a project's directory "
                               "or pass -p <dir>");
    }
    fs::path script = (*ws)->projectDir(*p) / "build.qz";
    auto text = readText(script);
    if (!text) {
        return std::unexpected("cannot read " + script.string());
    }
    return Target{std::move(*ws), p, std::move(script), std::move(*text)};
}

// Writes the edited script once it configures the project as intended, then updates qiln.lock from it.
int commit(const Args& a, Target& t, const std::string& text, bool resolve,
           const std::function<bool(const ProjectModel&)>& wanted, const std::string& done) {
    auto evaluated = t.ws->evaluateScript(t.project->dir, text);
    if (!evaluated) {
        return fail("the edited build.qz does not configure: " + evaluated.error());
    }
    if (!wanted(*evaluated)) {
        return fail(
            "the edit did not change what build.qz declares (is the call inside a condition?); edit it by hand");
    }
    if (auto w = writeAtomically(t.script, text); !w) {
        return fail(w.error());
    }
    if (resolve) {
        auto ws = loadWorkspace(a.dir, a.options);
        auto graph = ws ? (*ws)->graph(false).transform([](const LockGraph*) { return 0; })
                        : std::expected<int, std::string>(std::unexpected(ws.error()));
        if (!graph) {
            (void)writeAtomically(t.script, t.text);
            return fail(graph.error() + "\nbuild.qz is left as it was");
        }
    }
    std::printf("%s\n", done.c_str());
    return 0;
}

} // namespace

int add(const Args& a) {
    if (a.positional.size() != 1) {
        return fail("usage: qiln add <package>[@range] | <:project> [--test]");
    }
    auto c = coordinate(a.positional.front());
    if (!c) {
        return fail(c.error());
    }
    auto t = target(a);
    if (!t) {
        return fail(t.error());
    }
    auto& [subject, range] = *c;
    bool project = subject.starts_with(':');
    if (project) {
        const ProjectModel* dep = t->ws->model.findProject(subject);
        if (dep == nullptr || dep->kind != ArtifactKind::Library || dep == t->project) {
            return fail(std::format("{} is not another library project of this workspace", subject));
        }
    } else if (range.empty()) {
        auto repos = t->ws->repositories(false);
        auto versions = repos ? (*repos)->versions(subject) : std::unexpected(repos.error());
        if (!versions) {
            return fail(versions.error());
        }
        if (versions->empty()) {
            return fail(std::format("no repository of the workspace has {}; add one with p.repository(...)", subject));
        }
        range = "^" + quartz::formatVersion(versions->back());
    }
    std::string notation = project ? subject : subject + ":" + range;
    auto edited = edit::addDependency(t->text, {notation, a.testDependency});
    if (!edited) {
        return fail(edited.error());
    }
    std::string where = t->ws->displayPath(t->script);
    if (edited->change == edit::Change::Unchanged) {
        std::printf("%s already depends on %s\n", where.c_str(), notation.c_str());
        return 0;
    }
    bool test = a.testDependency;
    std::string done = std::format("{} {} {}in {}", edited->change == edit::Change::Added ? "added" : "changed to",
                                   notation, test ? "(test) " : "", where);
    return commit(
        a, *t, edited->text, !project, [&](const ProjectModel& p) { return declares(p, subject, range, test); }, done);
}

int remove(const Args& a) {
    if (a.positional.size() != 1) {
        return fail("usage: qiln remove <package> | <:project>");
    }
    auto c = coordinate(a.positional.front());
    if (!c) {
        return fail(c.error());
    }
    auto t = target(a);
    if (!t) {
        return fail(t.error());
    }
    std::string subject = c->first;
    auto edited = edit::removeDependency(t->text, subject);
    if (!edited) {
        return fail(edited.error());
    }
    std::string done = std::format("removed {} from {}", subject, t->ws->displayPath(t->script));
    return commit(
        a, *t, edited->text, !subject.starts_with(':'), [&](const ProjectModel& p) { return !names(p, subject); },
        done);
}

} // namespace qiln::cli
