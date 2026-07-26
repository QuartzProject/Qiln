#pragma once

#include "repo/Artifact.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace qiln {

struct RepositorySpec {
    std::string name;
    std::string url; // as written: a directory relative to the declaring script, file:// or https://
};

/// `implementation("org.example.text:^1.2")` or `project(":core")`, after validation.
struct DependencySpec {
    std::string configuration; // "implementation" or "test"
    std::string project;       // `:core` for a project of the workspace, else empty
    std::string name;          // package
    std::string range;
};

struct TaskSpec {
    std::string name;
    std::string description;
    std::string group;
    std::vector<std::string> dependsOn; // `build` in the same project or `:core:build`
    std::string plugin;                 // build plugin package whose sandbox runs `action`
    std::string action;
    std::map<std::string, std::string> options;
};

struct Limits {
    std::int64_t fuel = 100'000'000;
    std::int64_t memoryMiB = 64;
    std::int64_t maxDepth = 4096;
};

struct ProjectModel {
    std::string path; // `:` or `:core`
    std::string dir;  // relative to the workspace root, `/` separated, empty for the root
    std::string name;
    std::string group;
    std::string version = "0.1.0";
    std::string description;
    ArtifactKind kind = ArtifactKind::Library;
    std::string entry = "main";
    bool denyWarnings = false;
    bool hostOnly = false;
    std::vector<std::string> mainDirs; // empty means `src`
    std::vector<std::string> testDirs; // empty means `test`
    std::vector<std::string> interfaces;
    std::vector<RepositorySpec> repositories;
    std::vector<RepositorySpec> publishRepositories;
    std::vector<DependencySpec> dependencies;
    std::vector<std::string> exports;
    std::vector<std::string> capabilities;
    Limits limits;
    std::vector<std::string> plugins; // applied build plugins
    std::map<std::string, std::string> options;
    std::vector<TaskSpec> tasks; // declared by the script and its plugins

    [[nodiscard]] std::string packageName() const { return group.empty() ? name : group + "." + name; }
    [[nodiscard]] const TaskSpec* findTask(std::string_view task) const;
};

/// A build plugin the settings ask for: from the plugin repositories, or built from a directory of the workspace.
struct PluginRequest {
    std::string name;  // package, filled in later for a local plugin
    std::string range; // empty for a local plugin
    std::string dir;   // relative to the root, local plugins only
};

struct WorkspaceModel {
    std::string name;
    std::vector<std::string> includes; // project directories from settings.qz, in order
    std::vector<RepositorySpec> pluginRepositories;
    std::vector<PluginRequest> plugins;
    std::vector<ProjectModel> projects;

    [[nodiscard]] const ProjectModel* findProject(std::string_view path) const;
};

/// `:` for the root directory, `:a:b` for `a/b`.
std::string projectPath(std::string_view dir);

} // namespace qiln
