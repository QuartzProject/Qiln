#include "build/TaskHost.hpp"

#include "QilnApi.hpp"
#include "support/Files.hpp"

#include <cstdio>
#include <format>

namespace qiln {

namespace {

using quartz::vm::HostCall;

constexpr std::size_t kMaxFile = 64u << 20;
// Host work is paid for like Quartz work: one unit of fuel per 64 bytes moved.
constexpr std::size_t kBytesPerFuel = 64;

TaskEnvironment& env(void* user) { return *static_cast<TaskEnvironment*>(user); }

std::string_view str(const HostCall& call, std::size_t i) {
    return static_cast<const quartz::vm::Str*>(call.args[i].p)->view();
}

bool fail(HostCall& call, std::string message) {
    call.error = std::move(message);
    return false;
}

bool returnStr(HostCall& call, std::string_view text) {
    call.result.p = call.string(text);
    return call.result.p != nullptr || fail(call, "out of memory");
}

// A path the task names, resolved inside `base`; symbolic links may not lead out of it.
std::expected<fs::path, std::string> inside(const fs::path& base, std::string_view path) {
    if (!safeRelativePath(path)) {
        return std::unexpected(std::format("'{}' is not a relative path inside the directory", path));
    }
    std::error_code ec;
    fs::path canonicalBase = fs::weakly_canonical(base, ec);
    fs::path full = fs::weakly_canonical(base / std::string(path), ec);
    std::string b = canonicalBase.generic_string();
    std::string f = full.generic_string();
    if (ec || !(f.size() > b.size() && f.starts_with(b) && f[b.size()] == '/')) {
        return std::unexpected(std::format("'{}' leads outside the directory", path));
    }
    return full;
}

bool readInto(HostCall& call, const fs::path& base, bool binary) {
    auto path = inside(base, str(call, 1));
    if (!path) {
        return fail(call, path.error());
    }
    auto bytes = readLimited(*path, kMaxFile);
    if (!bytes) {
        return fail(call, bytes.error());
    }
    if (!*bytes) {
        call.result.p = nullptr;
        return true;
    }
    if (!call.charge(static_cast<std::int64_t>((*bytes)->size() / kBytesPerFuel))) {
        return false;
    }
    if (!binary) {
        return returnStr(call, textOf(**bytes));
    }
    call.result.p = call.bytes(textOf(**bytes));
    return call.result.p != nullptr || fail(call, "out of memory");
}

bool name(HostCall& call, void* user) { return returnStr(call, env(user).task->name); }

bool projectPath(HostCall& call, void* user) { return returnStr(call, env(user).project->path); }

bool projectName(HostCall& call, void* user) { return returnStr(call, env(user).project->name); }

bool projectVersion(HostCall& call, void* user) { return returnStr(call, env(user).project->version); }

bool packageName(HostCall& call, void* user) { return returnStr(call, env(user).project->packageName()); }

bool artifact(HostCall& call, void* user) { return returnStr(call, env(user).artifact); }

bool option(HostCall& call, void* user) {
    std::string key(str(call, 1));
    const TaskEnvironment& e = env(user);
    if (auto it = e.task->options.find(key); it != e.task->options.end()) {
        return returnStr(call, it->second);
    }
    if (auto it = e.project->options.find(key); it != e.project->options.end()) {
        return returnStr(call, it->second);
    }
    call.result.p = nullptr;
    return true;
}

bool sources(HostCall& call, void* user) {
    const auto& imports = call.sandbox.program().module->imports;
    quartz::ir::TypeId listType = quartz::ir::kVoid;
    for (const auto& imp : imports) {
        if (imp.name == "TaskContext.sources") {
            listType = imp.result;
        }
    }
    quartz::vm::List* list = call.sandbox.hostList(listType);
    if (list == nullptr) {
        return fail(call, "out of memory");
    }
    for (const std::string& s : env(user).sources) {
        quartz::vm::Slot item{};
        item.p = call.string(s);
        if (item.p == nullptr || !call.sandbox.hostListPush(list, std::span(&item, 1))) {
            return fail(call, "out of memory");
        }
    }
    call.result.p = list;
    return true;
}

bool exists(HostCall& call, void* user) {
    auto path = inside(env(user).projectDir, str(call, 1));
    std::error_code ec;
    call.result.u = path && fs::exists(*path, ec) ? 1 : 0;
    return true;
}

bool readText(HostCall& call, void* user) { return readInto(call, env(user).projectDir, false); }

bool readBytes(HostCall& call, void* user) { return readInto(call, env(user).projectDir, true); }

bool readBuild(HostCall& call, void* user) { return readInto(call, env(user).buildDir, true); }

bool writeBuild(HostCall& call, void* user) {
    auto path = inside(env(user).buildDir, str(call, 1));
    if (!path) {
        return fail(call, path.error());
    }
    std::string_view data = str(call, 2);
    if (!call.charge(static_cast<std::int64_t>(data.size() / kBytesPerFuel))) {
        return false;
    }
    call.result.u = writeAtomically(*path, data) ? 1 : 0;
    return true;
}

bool log(HostCall& call, void* user) {
    std::string_view text = str(call, 0);
    std::printf("  [%s] %.*s\n", env(user).task->name.c_str(), static_cast<int>(text.size()), text.data());
    return true;
}

} // namespace

std::expected<std::shared_ptr<const quartz::vm::Program>, std::string>
compilePluginTasks(const std::string& plugin, const std::vector<quartz::SourceInput>& sources) {
    std::vector<quartz::SourceInput> inputs = {{"<qiln>/task.qzi", std::string(api::kTaskQzi), true}};
    inputs.insert(inputs.end(), sources.begin(), sources.end());
    quartz::CompileOutput out = quartz::compile(inputs);
    if (!out.program) {
        return std::unexpected(
            std::format("the tasks of build plugin {} do not compile:\n{}", plugin, out.diagnostics));
    }
    return out.program;
}

std::expected<std::string, std::string> runPluginTask(const std::shared_ptr<const quartz::vm::Program>& program,
                                                      TaskEnvironment& e) {
    const TaskSpec& t = *e.task;
    std::uint32_t f = program->findFunction(t.action);
    const auto& types = program->module->types;
    std::string wanted = std::format("pub fn {}(t: TaskContext) -> Result<str, str>", t.action);
    if (f == ~0U) {
        return std::unexpected(std::format("build plugin {} has no task function '{}'", t.plugin, t.action));
    }
    const auto& fn = program->functions[f];
    if (fn.params.size() != 1 || types.kind(fn.params[0]) != quartz::ir::TypeKind::Handle ||
        types.name(fn.result) != "Result<str, str>" || fn.isSuspend) {
        return std::unexpected(std::format("'{}' of {} must be declared as {}", t.action, t.plugin, wanted));
    }
    quartz::vm::Limits limits;
    limits.fuelPerCall = kTaskFuel;
    limits.memoryBytes = kTaskMemory;
    quartz::vm::Sandbox sb(program, limits);

    struct Entry {
        std::string_view name;
        quartz::vm::HostFn fn;
    };

    static constexpr Entry kEntries[] = {
        {"TaskContext.name", name},
        {"TaskContext.projectPath", projectPath},
        {"TaskContext.projectName", projectName},
        {"TaskContext.projectVersion", projectVersion},
        {"TaskContext.packageName", packageName},
        {"TaskContext.artifact", artifact},
        {"TaskContext.option", option},
        {"TaskContext.sources", sources},
        {"TaskContext.exists", exists},
        {"TaskContext.readText", readText},
        {"TaskContext.readBytes", readBytes},
        {"TaskContext.readBuild", readBuild},
        {"TaskContext.writeBuild", writeBuild},
        {"TaskContext.writeBuildText", writeBuild},
        {"org.quartzlang.qiln.task.log", log},
    };
    for (const Entry& entry : kEntries) {
        sb.bind(entry.name, entry.fn, &e);
    }
    if (auto s = sb.start(); !s) {
        return std::unexpected(s.error().describe());
    }
    quartz::vm::Slot handle{};
    handle.u = 1;
    auto r = sb.call(f, std::span(&handle, 1));
    if (!r) {
        return std::unexpected(r.error().describe());
    }
    auto slots = sb.resultSlots(fn.resultSlots);
    std::string_view text = static_cast<const quartz::vm::Str*>(slots[1].p)->view();
    if (slots[0].i != 0) {
        return std::unexpected(std::string(text));
    }
    return std::string(text);
}

} // namespace qiln
