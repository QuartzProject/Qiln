#pragma once

#include "model/Model.hpp"
#include "resolve/Lockfile.hpp"
#include "script/Script.hpp"

#include <quartz/Quartz.hpp>

#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace qiln {

namespace fs = std::filesystem;

class Repositories;

struct Options {
    Properties properties; // -Pkey=value
    bool noCache = false;  // evaluate the scripts even when the cached configuration is current
    bool update = false;   // resolve to the newest versions the ranges allow
    bool locked = false;   // fail instead of changing qiln.lock
    bool allowKeyChange = false;
    bool quiet = false;
    bool allowInsecureLocalhost = false; // plain http to the loopback address, for test repositories
};

/// A build plugin ready to use: the API compiled into build scripts and the task sources for its own sandbox.
struct LoadedPlugin {
    std::string name;
    std::string version;
    std::string origin; // a directory of the workspace, or `from <repository>`
    std::vector<quartz::SourceInput> api;
    std::vector<quartz::SourceInput> tasks; // its sources plus the libraries it depends on
};

/// The evaluated workspace and everything later steps share: plugins, the lock and the repositories.
class Workspace {
public:
    fs::path root;
    bool hasSettings = false;
    WorkspaceModel model;
    Options options;
    bool fromCache = false;

    [[nodiscard]] fs::path projectDir(const ProjectModel& p) const { return p.dir.empty() ? root : root / p.dir; }

    [[nodiscard]] std::string displayPath(const fs::path& path) const;

    /// Every requested build plugin, resolved, fetched and verified on first use.
    std::expected<const std::vector<LoadedPlugin>*, std::string> plugins();
    /// One graph of qiln.lock, resolved again first when its requirements changed (or with `--update`).
    std::expected<const LockGraph*, std::string> graph(bool pluginGraph);
    /// The verified archive of a package locked in that graph.
    std::expected<std::vector<std::uint8_t>, std::string> archive(const Locked& p, bool pluginGraph);
    /// The repositories of the project graph (every project's) or of the plugin graph.
    std::expected<Repositories*, std::string> repositories(bool pluginGraph);
    /// Runs the `build.qz` text of the project in `dir` (relative to the root) against the requested plugins.
    std::expected<ProjectModel, std::string> evaluateScript(const std::string& dir, const std::string& text);

    Workspace();
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

private:
    std::optional<std::vector<LoadedPlugin>> plugins_;
    std::optional<Lockfile> lock_;
    bool checked_[2] = {false, false};
    std::unique_ptr<Repositories> repositories_[2];
    std::vector<Requirement> requirements(bool pluginGraph) const;
    std::expected<void, std::string> evaluateLocalPlugins();
    std::expected<Lockfile*, std::string> loadLock();
};

/// Finds the root above `start` (the nearest `settings.qz`, else the nearest `build.qz`) and configures it.
std::expected<std::unique_ptr<Workspace>, std::string> loadWorkspace(const fs::path& start, const Options& options);

/// `.qiln/api/` at the root: the build interface, its prelude, the task interface and the requested plugins' `api/`
/// sources, for an editor to resolve `build.qz` and `settings.qz`.
std::expected<void, std::string> writeEditorApi(Workspace& ws);

/// A valid project name made from the last segment of `dir` (`text-utils` becomes `text_utils`).
std::string defaultProjectName(std::string_view dir);

/// The project whose directory holds `dir`, or null when it is the root of a multi-project workspace.
const ProjectModel* currentProject(const Workspace& ws, const fs::path& dir);

} // namespace qiln
