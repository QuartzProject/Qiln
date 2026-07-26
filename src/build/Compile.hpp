#pragma once

#include "workspace/Workspace.hpp"

#include <quartz/Quartz.hpp>

#include <expected>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace qiln {

/// A host plugin (a `.qzb` package) a program links to at run time, with its own sandbox.
struct RuntimePlugin {
    std::string name;
    std::shared_ptr<const quartz::vm::Program> program;
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

/// Compiles projects once per run and remembers the result.
class Compiler {
public:
    explicit Compiler(Workspace& ws) : ws_(ws) {}

    std::expected<SourceSet, std::string> sources(const ProjectModel& p, bool tests);
    /// The project compiled with its tests or without; diagnostics are printed the first time.
    std::expected<const Compiled*, std::string> compile(const ProjectModel& p, bool tests);
    /// A build plugin's two halves: the API against the build API and the tasks against the task API.
    std::expected<void, std::string> checkBuildPlugin(const ProjectModel& p);
    /// `package.toml` as the Quartz core reads it, carried in archives and modules.
    static std::string packageToml(const ProjectModel& p);

private:
    Workspace& ws_;
    std::map<std::pair<std::string, bool>, Compiled> done_;
    std::map<std::string, std::vector<quartz::SourceInput>> libraries_;
    std::map<std::string, RuntimePlugin> runtimePlugins_;

    std::expected<void, std::string> addPackages(const std::vector<std::string>& roots, SourceSet& out);
};

} // namespace qiln
