#pragma once

#include "model/Model.hpp"
#include "script/Script.hpp"

#include <quartz/Quartz.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace qiln::host {

/// What the build API's host functions act on while one script runs.
struct Context {
    WorkspaceModel* workspace = nullptr;
    ProjectModel* project = nullptr;
    const Properties* properties = nullptr;
    const std::vector<std::string>* requested = nullptr; // build plugins settings.qz asked for
    std::string scriptName;
};

inline std::string_view str(const quartz::vm::HostCall& call, std::size_t i) {
    return static_cast<const quartz::vm::Str*>(call.args[i].p)->view();
}

/// Sets the error a trap reports at the script line that made the call.
inline bool fail(quartz::vm::HostCall& call, std::string message) {
    call.error = std::move(message);
    return false;
}

inline bool returnStr(quartz::vm::HostCall& call, std::string_view text) {
    call.result.p = call.string(text);
    return call.result.p != nullptr || fail(call, "out of memory");
}

inline Context& context(void* user) { return *static_cast<Context*>(user); }

bool validName(std::string_view s);       // a package segment: `[a-z][a-z0-9_]*`
bool validIdentifier(std::string_view s); // a Quartz identifier
bool validTaskName(std::string_view s);   // `[A-Za-z][A-Za-z0-9_-]*`
bool validTaskRef(std::string_view s);    // a task name, optionally after a project path (`:core:build`)
/// `org.example.text:^1.2` split into package and range, both checked.
std::expected<std::pair<std::string, std::string>, std::string> parseNotation(std::string_view notation);

void bindSettings(quartz::vm::Sandbox& sb, Context& ctx);
void bindProject(quartz::vm::Sandbox& sb, Context& ctx);
void bindCommon(quartz::vm::Sandbox& sb, Context& ctx);

} // namespace qiln::host
