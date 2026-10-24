#pragma once

#include "build/Compile.hpp"
#include "build/Stamp.hpp"
#include "workspace/Workspace.hpp"

#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace qiln {

struct RunOptions {
    std::string testFilter;
    std::string publishTo;  // overrides the project's publish repositories
    std::string signingKey; // seed file; else $QILN_SIGNING_KEY, else ~/.qiln/signing.key
    bool denyWarnings = false;
    std::size_t jobs = 0; // tasks at once; 0 is one per hardware thread
    bool rerun = false;   // run every task even when it is up to date
};

/// A task of one project, built in or declared by its script or plugins.
struct TaskRef {
    const ProjectModel* project = nullptr;
    std::string name;
    std::string description;
    std::string group;
    std::vector<std::string> dependsOn;
    const TaskSpec* spec = nullptr; // null for a built-in task nobody configured
    bool builtin = false;
};

/// Every task of `p`, built-in ones first.
std::vector<TaskRef> tasksOf(const ProjectModel& p);

/// `:core:build`, or `:build` in the root project.
std::string qualifiedName(const ProjectModel& p, std::string_view task);

class TaskRunner {
public:
    TaskRunner(Workspace& ws, RunOptions options);
    ~TaskRunner();

    /// Runs the requested tasks (`build`, `:core:test`...) with their dependencies; the exit code of `run`, or 0.
    std::expected<int, std::string> run(const std::vector<std::string>& requests, const ProjectModel* current);

private:
    struct Outcome {
        int code = 0;
        bool upToDate = false;
    };

    Workspace& ws_;
    RunOptions options_;
    Compiler compiler_;
    std::mutex mutex_; // the members below
    std::map<std::string, std::shared_ptr<std::once_flag>> pluginOnce_;
    std::map<std::string, std::expected<std::shared_ptr<const quartz::vm::Program>, std::string>> pluginPrograms_;
    std::set<std::string> warnedResult_;

    std::expected<Outcome, std::string> execute(const TaskRef& t);
    std::expected<void, std::string> prepare(const std::vector<const TaskRef*>& order);
    std::expected<Outcome, std::string> check(const ProjectModel& p);
    std::expected<Outcome, std::string> build(const ProjectModel& p);
    std::expected<Outcome, std::string> test(const ProjectModel& p);
    std::expected<int, std::string> runEntry(const ProjectModel& p);
    std::expected<void, std::string> publish(const ProjectModel& p);
    std::expected<void, std::string> clean(const ProjectModel& p);
    std::expected<Outcome, std::string> pluginTask(const TaskRef& t);
    std::expected<std::shared_ptr<const quartz::vm::Program>, std::string> pluginProgram(const LoadedPlugin& plugin);

    /// What a compiling task depends on: every source it compiles, the linked packages and the options.
    std::expected<Hash32, std::string> fingerprint(const ProjectModel& p, std::string_view task);
    /// True when the task's stamp holds `inputs` (then what a plugin task read) and its outputs are unchanged.
    bool upToDate(const ProjectModel& p, std::string_view task, const std::pair<std::string, Hash32>& inputs);
    void saveStamp(const ProjectModel& p, std::string_view task, const Stamp& stamp);
};

} // namespace qiln
