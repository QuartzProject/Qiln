#pragma once

#include "workspace/Workspace.hpp"

#include <quartz/Quartz.hpp>

#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace qiln {

/// A host plugin (a `.qzb` package) a program links to at run time, with its own sandbox.
struct RuntimePlugin {
    std::string name;
    std::shared_ptr<const quartz::vm::Program> program;
    std::string checksum; // its locked BLAKE2b, part of the up-to-date check of what links it
};

/// Everything one compilation of a project takes.
struct SourceSet {
    std::vector<quartz::SourceInput> inputs; // interfaces, the project's sources, then what it depends on
    std::vector<std::string> own;            // the project's own source files, relative to the root
    std::vector<RuntimePlugin> plugins;
};

struct Compiled {
    std::shared_ptr<const quartz::vm::Program> program;
    std::vector<std::uint8_t> binary; // the `.qzb` module, for applications and host plugins
    std::size_t warnings = 0;
    std::vector<RuntimePlugin> plugins;
};

/// Interface of the runner `qiln run` and `qiln test` provide, unless a project is host only.
inline constexpr std::string_view kRunnerInterfaceName = "<qiln>/runner.qzi";
inline constexpr std::string_view kRunnerInterface = R"(fn print(text: str)
@handle
@awaitable
struct Delay {}
impl int {
    @property
    fn ticks(self) -> Delay
}
)";

/// Compiles projects once per run and remembers the result; tasks on several threads share it.
class Compiler {
public:
    Compiler(Workspace& ws, bool denyWarnings) : ws_(ws), denyWarnings_(denyWarnings) {}

    /// Everything the project compiles from, read once per run.
    std::expected<const SourceSet*, std::string> sources(const ProjectModel& p, bool tests);
    /// The project compiled with its tests or without; diagnostics are printed the first time.
    std::expected<const Compiled*, std::string> compile(const ProjectModel& p, bool tests);
    /// A build plugin's two halves: the API against the build API and the tasks against the task API.
    std::expected<void, std::string> checkBuildPlugin(const ProjectModel& p);
    /// The sources of a build plugin's `api/` directory.
    std::expected<std::vector<quartz::SourceInput>, std::string> apiSources(const ProjectModel& p) const;
    /// `package.toml` as the Quartz core reads it, carried in archives and modules.
    static std::string packageToml(const ProjectModel& p);
    /// The warning levels a project compiles with: `*=error` when warnings are denied, then its own rules.
    [[nodiscard]] std::vector<quartz::RuleSetting> rulesFor(const ProjectModel& p) const;

private:
    template <class T>
    struct Once {
        std::once_flag flag;
        std::expected<T, std::string> value = std::unexpected(std::string());
    };

    using Key = std::pair<std::string, bool>;

    Workspace& ws_;
    bool denyWarnings_ = false;
    std::mutex mutex_; // the maps below, not their entries
    std::map<Key, std::unique_ptr<Once<SourceSet>>> sources_;
    std::map<Key, std::unique_ptr<Once<Compiled>>> compiled_;
    std::mutex packages_; // the workspace's lazy downloads and the two caches below
    std::map<std::string, std::vector<quartz::SourceInput>> libraries_;
    std::map<std::string, RuntimePlugin> runtimePlugins_;

    template <class T>
    Once<T>& slot(std::map<Key, std::unique_ptr<Once<T>>>& map, const Key& key);
    std::expected<SourceSet, std::string> readSources(const ProjectModel& p, bool tests);
    std::expected<Compiled, std::string> compileNow(const ProjectModel& p, bool tests);
    std::expected<void, std::string> addPackages(const std::vector<std::string>& roots, SourceSet& out);
};

} // namespace qiln
