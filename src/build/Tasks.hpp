#pragma once

#include "build/Compile.hpp"
#include "workspace/Workspace.hpp"

#include <expected>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace qiln {

struct RunOptions {
    std::string testFilter;
    std::string publishTo;  // overrides the project's publish repositories
    std::string signingKey; // seed file; else $QILN_SIGNING_KEY, else ~/.qiln/signing.key
    bool denyWarnings = false;
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

class TaskRunner {
public:
    TaskRunner(Workspace& ws, RunOptions options);
    ~TaskRunner();

    /// Runs the requested tasks (`build`, `:core:test`...) with their dependencies; the exit code of `run`, or 0.
    std::expected<int, std::string> run(const std::vector<std::string>& requests, const ProjectModel* current);

private:
    Workspace& ws_;
    RunOptions options_;
    Compiler compiler_;
    std::map<std::string, std::string> artifacts_; // project path -> artifact relative to its build directory
    std::map<std::string, std::shared_ptr<const quartz::vm::Program>> pluginPrograms_;
    std::set<std::string> warnedResult_;

    std::expected<int, std::string> execute(const TaskRef& t);
    std::expected<void, std::string> check(const ProjectModel& p);
    std::expected<void, std::string> build(const ProjectModel& p);
    std::expected<void, std::string> test(const ProjectModel& p);
    std::expected<int, std::string> runEntry(const ProjectModel& p);
    std::expected<void, std::string> publish(const ProjectModel& p);
    std::expected<void, std::string> clean(const ProjectModel& p);
    std::expected<void, std::string> pluginTask(const TaskRef& t);
};

} // namespace qiln
