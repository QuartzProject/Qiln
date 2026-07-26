#pragma once

#include "model/Model.hpp"

#include <quartz/Quartz.hpp>

#include <expected>
#include <map>
#include <string>
#include <vector>

namespace qiln {

using Properties = std::map<std::string, std::string>;

/// A `settings.qz` or `build.qz`: its name in diagnostics (relative to the root) and its text.
struct Script {
    std::string name;
    std::string text;
};

/// Runs `settings(s: Settings)`, filling the workspace's name, projects, plugin repositories and plugins.
std::expected<void, std::string> evaluateSettings(const Script& script, WorkspaceModel& ws, const Properties& props);

/// Runs `build(p: Project)` against the build API and the API sources of the requested build plugins.
std::expected<void, std::string> evaluateBuild(const Script& script, ProjectModel& project,
                                               const std::vector<quartz::SourceInput>& pluginApis,
                                               const std::vector<std::string>& requestedPlugins,
                                               const Properties& props);

/// The fixed budget of a configuration script: enough for any configuration, small enough to stop a runaway loop.
inline constexpr std::int64_t kScriptFuel = 20'000'000;
inline constexpr std::size_t kScriptMemory = 16u << 20;

} // namespace qiln
