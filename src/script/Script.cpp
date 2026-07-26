#include "script/Script.hpp"

#include "QilnApi.hpp"
#include "script/Host.hpp"

#include <cstdio>
#include <format>

namespace qiln {

namespace {

constexpr std::string_view kQziName = "<qiln>/qiln.qzi";
constexpr std::string_view kPreludeName = "<qiln>/qiln.qz";

// The script's entry point must take exactly the handle the host passes: `fn build(p: Project)`.
std::expected<std::uint32_t, std::string> entryPoint(const quartz::vm::Program& program, std::string_view script,
                                                     std::string_view function, std::string_view type) {
    std::uint32_t f = program.findFunction(function);
    std::string wanted = std::format("fn {}({}: {})", function, type == "Settings" ? "s" : "p", type);
    if (f == ~0U) {
        return std::unexpected(std::format("{}: no '{}'; a {} declares {}", script, wanted, script, wanted));
    }
    const auto& fn = program.functions[f];
    const auto& types = program.module->types;
    bool ok = fn.params.size() == 1 && fn.result == quartz::ir::kVoid && !fn.isSuspend &&
              types.kind(fn.params[0]) == quartz::ir::TypeKind::Handle && types.name(fn.params[0]).ends_with(type);
    if (!ok) {
        return std::unexpected(std::format("{}: '{}' must be declared as {}", script, function, wanted));
    }
    return f;
}

std::expected<void, std::string> run(const Script& script, std::vector<quartz::SourceInput> inputs,
                                     std::string_view function, std::string_view type, host::Context& ctx) {
    inputs.insert(inputs.begin(), {std::string(kPreludeName), std::string(api::kQilnQz), true});
    inputs.insert(inputs.begin(), {std::string(kQziName), std::string(api::kQilnQzi), true});
    inputs.push_back({script.name, script.text});
    quartz::CompileOutput out = quartz::compile(inputs);
    if (!out.program) {
        return std::unexpected(out.diagnostics);
    }
    if (!out.diagnostics.empty()) {
        std::fputs(out.diagnostics.c_str(), stderr);
    }
    auto f = entryPoint(*out.program, script.name, function, type);
    if (!f) {
        return std::unexpected(f.error());
    }
    quartz::vm::Limits limits;
    limits.fuelPerCall = kScriptFuel;
    limits.memoryBytes = kScriptMemory;
    limits.tier = quartz::vm::TierMode::Interpreter;
    quartz::vm::Sandbox sb(out.program, limits);
    host::bindCommon(sb, ctx);
    host::bindSettings(sb, ctx);
    host::bindProject(sb, ctx);
    if (auto s = sb.start(); !s) {
        return std::unexpected(std::format("{}: {}", script.name, s.error().describe()));
    }
    quartz::vm::Slot handle{};
    handle.u = 1;
    if (auto r = sb.call(*f, std::span(&handle, 1)); !r) {
        return std::unexpected(std::format("{}: {}", script.name, r.error().describe()));
    }
    return {};
}

} // namespace

std::expected<void, std::string> evaluateSettings(const Script& script, WorkspaceModel& ws, const Properties& props) {
    std::vector<std::string> none;
    host::Context ctx{&ws, nullptr, &props, &none, script.name};
    return run(script, {}, "settings", "Settings", ctx);
}

std::expected<void, std::string> evaluateBuild(const Script& script, ProjectModel& project,
                                               const std::vector<quartz::SourceInput>& pluginApis,
                                               const std::vector<std::string>& requestedPlugins,
                                               const Properties& props) {
    host::Context ctx{nullptr, &project, &props, &requestedPlugins, script.name};
    return run(script, pluginApis, "build", "Project", ctx);
}

} // namespace qiln
