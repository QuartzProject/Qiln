#pragma once

#include "model/Model.hpp"

#include <quartz/Quartz.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace qiln {

/// What a plugin task may touch: the project directory to read and its build directory to write.
struct TaskEnvironment {
    const ProjectModel* project = nullptr;
    const TaskSpec* task = nullptr;
    std::filesystem::path projectDir;
    std::filesystem::path buildDir;
    std::string artifact;             // relative to the build directory, empty before `build` ran
    std::vector<std::string> sources; // main sources, relative to the project directory
};

/// The budget of a plugin task: larger than a build script's, since tasks compress and read files.
inline constexpr std::int64_t kTaskFuel = 2'000'000'000;
inline constexpr std::size_t kTaskMemory = 256u << 20;

/// Compiles a build plugin's task sources against the task API.
std::expected<std::shared_ptr<const quartz::vm::Program>, std::string>
compilePluginTasks(const std::string& plugin, const std::vector<quartz::SourceInput>& sources);

struct TaskOutcome {
    std::string summary;           // the Ok of a deprecated `Result<str, str>` action
    bool deprecatedResult = false; // the action still returns `Result<str, str>`
};

/// Runs `fn <action>(t: TaskContext) -> Result<(), str>` in a new sandbox; an Err fails the task with its text.
std::expected<TaskOutcome, std::string> runPluginTask(const std::shared_ptr<const quartz::vm::Program>& program,
                                                      TaskEnvironment& env);

} // namespace qiln
