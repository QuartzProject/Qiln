#include "script/Host.hpp"
#include "support/Files.hpp"

#include <algorithm>
#include <format>

namespace qiln::host {

namespace {

using quartz::vm::HostCall;

constexpr std::string_view kBuiltinTasks[] = {"check", "test", "build", "run", "publish", "clean"};

// A settings script never receives a Project handle, so `project` is always set when these run.
ProjectModel& project(HostCall&, void* user) { return *context(user).project; }

std::string text(HostCall& call, std::size_t i) { return std::string(str(call, i)); }

bool getName(HostCall& call, void* user) { return returnStr(call, project(call, user).name); }

bool getGroup(HostCall& call, void* user) { return returnStr(call, project(call, user).group); }

bool getVersion(HostCall& call, void* user) { return returnStr(call, project(call, user).version); }

bool getDescription(HostCall& call, void* user) { return returnStr(call, project(call, user).description); }

bool getEntry(HostCall& call, void* user) { return returnStr(call, project(call, user).entry); }

bool getPath(HostCall& call, void* user) { return returnStr(call, project(call, user).path); }

bool getPackageName(HostCall& call, void* user) { return returnStr(call, project(call, user).packageName()); }

bool getKind(HostCall& call, void* user) {
    call.result.i = static_cast<std::int64_t>(project(call, user).kind);
    return true;
}

bool getDenyWarnings(HostCall& call, void* user) {
    call.result.u = project(call, user).denyWarnings ? 1 : 0;
    return true;
}

bool getHostOnly(HostCall& call, void* user) {
    call.result.u = project(call, user).hostOnly ? 1 : 0;
    return true;
}

bool setName(HostCall& call, void* user) {
    if (!validName(str(call, 1))) {
        return fail(call, std::format("'{}' is not a project name (lower case letters, digits and _)", str(call, 1)));
    }
    project(call, user).name = text(call, 1);
    return true;
}

bool setGroup(HostCall& call, void* user) {
    std::string group = text(call, 1);
    if (!group.empty() && !coordinatesOf(group + ".x")) {
        return fail(call, std::format("'{}' is not a group like org.example", group));
    }
    project(call, user).group = std::move(group);
    return true;
}

bool setVersion(HostCall& call, void* user) {
    auto v = quartz::parseVersion(str(call, 1));
    if (!v || quartz::formatVersion(*v) != str(call, 1)) {
        return fail(call, std::format("'{}' is not a major.minor.patch version", str(call, 1)));
    }
    project(call, user).version = text(call, 1);
    return true;
}

bool setDescription(HostCall& call, void* user) {
    if (str(call, 1).size() > 1024) {
        return fail(call, "a description has at most 1024 bytes");
    }
    project(call, user).description = text(call, 1);
    return true;
}

bool setKind(HostCall& call, void* user) {
    if (call.args[1].i < 0 || call.args[1].i > static_cast<std::int64_t>(ArtifactKind::BuildPlugin)) {
        return fail(call, "unknown kind");
    }
    project(call, user).kind = static_cast<ArtifactKind>(call.args[1].i);
    return true;
}

bool setEntry(HostCall& call, void* user) {
    if (!validIdentifier(str(call, 1))) {
        return fail(call, std::format("'{}' is not a function name", str(call, 1)));
    }
    project(call, user).entry = text(call, 1);
    return true;
}

bool setDenyWarnings(HostCall& call, void* user) {
    project(call, user).denyWarnings = call.args[1].u != 0;
    return true;
}

bool setRuleLevel(HostCall& call, void* user) {
    std::string_view rule = str(call, 1);
    auto rules = quartz::warningRules();
    if (rule != "*" && std::find(rules.begin(), rules.end(), rule) == rules.end()) {
        std::string known;
        for (std::string_view r : rules) {
            known += std::format("{}{}", known.empty() ? "" : ", ", r);
        }
        return fail(call, std::format("'{}' is not a warning rule; the rules are * (all), {}", rule, known));
    }
    if (call.args[2].i < 0 || call.args[2].i > 2) {
        return fail(call, "unknown rule level");
    }
    auto& list = project(call, user).rules;
    if (list.size() >= 256) {
        return fail(call, "too many warning rules");
    }
    list.push_back({std::string(rule), static_cast<std::uint8_t>(call.args[2].i)});
    return true;
}

bool setHostOnly(HostCall& call, void* user) {
    project(call, user).hostOnly = call.args[1].u != 0;
    return true;
}

bool sourceDir(HostCall& call, void* user) {
    std::string set = text(call, 1);
    std::string dir = text(call, 2);
    if (set != "main" && set != "test") {
        return fail(call, std::format("source set '{}' is neither main nor test", set));
    }
    if (!safeRelativePath(dir)) {
        return fail(call, std::format("'{}' is not a directory inside the project", dir));
    }
    auto& dirs = set == "main" ? project(call, user).mainDirs : project(call, user).testDirs;
    if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) {
        dirs.push_back(std::move(dir));
    }
    return true;
}

bool hostInterface(HostCall& call, void* user) {
    std::string path = text(call, 1);
    if (!safeRelativePath(path) || !path.ends_with(".qzi")) {
        return fail(call, std::format("'{}' is not a .qzi file inside the project", path));
    }
    project(call, user).interfaces.push_back(std::move(path));
    return true;
}

bool addRepository(HostCall& call, void* user) {
    if (str(call, 2).empty() || str(call, 2).size() > 1024) {
        return fail(call, "a repository needs a directory or an https:// URL");
    }
    project(call, user).repositories.push_back({text(call, 1), text(call, 2)});
    return true;
}

bool addPublishRepository(HostCall& call, void* user) {
    if (str(call, 2).empty() || str(call, 2).size() > 1024) {
        return fail(call, "a repository needs a directory or an https:// URL");
    }
    project(call, user).publishRepositories.push_back({text(call, 1), text(call, 2)});
    return true;
}

bool addDependency(HostCall& call, void* user) {
    std::string configuration = text(call, 1);
    std::string_view notation = str(call, 2);
    if (configuration != "implementation" && configuration != "test") {
        return fail(call, std::format("configuration '{}' is neither implementation nor test", configuration));
    }
    DependencySpec d{configuration, "", "", ""};
    if (notation.starts_with(':')) {
        if (notation == ":" || !validTaskRef(std::string(notation) + ":x")) {
            return fail(call, std::format("'{}' is not a project path like :core", notation));
        }
        d.project = std::string(notation);
    } else {
        auto n = parseNotation(notation);
        if (!n) {
            return fail(call, n.error());
        }
        d.name = n->first;
        d.range = n->second;
    }
    project(call, user).dependencies.push_back(std::move(d));
    return true;
}

bool addExport(HostCall& call, void* user) {
    if (!validIdentifier(str(call, 1))) {
        return fail(call, std::format("'{}' is not a function name", str(call, 1)));
    }
    project(call, user).exports.push_back(text(call, 1));
    return true;
}

bool addCapability(HostCall& call, void* user) {
    std::string name = text(call, 1);
    std::string probe = name;
    std::replace(probe.begin(), probe.end(), '.', '_');
    if (!validIdentifier(probe)) {
        return fail(call, std::format("'{}' is not a capability name", name));
    }
    project(call, user).capabilities.push_back(std::move(name));
    return true;
}

bool setLimits(HostCall& call, void* user) {
    std::int64_t fuel = call.args[1].i;
    std::int64_t memory = call.args[2].i;
    std::int64_t depth = call.args[3].i;
    if (fuel < 1000 || fuel > 1'000'000'000'000 || memory < 1 || memory > 4096 || depth < 16 || depth > 1'000'000) {
        return fail(call, "limits: fuel from 1000 to 10^12, memoryMiB from 1 to 4096, maxDepth from 16 to 10^6");
    }
    project(call, user).limits = {fuel, memory, depth};
    return true;
}

bool task(HostCall& call, void* user) {
    std::string name = text(call, 1);
    if (!validTaskName(name)) {
        return fail(call, std::format("'{}' is not a task name", name));
    }
    auto& tasks = project(call, user).tasks;
    auto it = std::find_if(tasks.begin(), tasks.end(), [&](const TaskSpec& t) { return t.name == name; });
    if (it == tasks.end()) {
        if (tasks.size() >= 1024) {
            return fail(call, "too many tasks");
        }
        tasks.push_back(TaskSpec{name, "", "", {}, "", "", {}});
        it = tasks.end() - 1;
    }
    call.result.u = static_cast<std::uint64_t>(it - tasks.begin()) + 1;
    return true;
}

bool applyPlugin(HostCall& call, void* user) {
    std::string name = text(call, 1);
    const auto& requested = *context(user).requested;
    if (std::find(requested.begin(), requested.end(), name) == requested.end()) {
        std::string list;
        for (const std::string& r : requested) {
            list += (list.empty() ? "" : ", ") + r;
        }
        return fail(call, std::format("build plugin {} is not requested in settings.qz (requested: {})", name,
                                      list.empty() ? "none" : list));
    }
    auto& plugins = project(call, user).plugins;
    if (std::find(plugins.begin(), plugins.end(), name) == plugins.end()) {
        plugins.push_back(std::move(name));
    }
    return true;
}

bool hasPlugin(HostCall& call, void* user) {
    const auto& plugins = project(call, user).plugins;
    call.result.u = std::find(plugins.begin(), plugins.end(), str(call, 1)) != plugins.end() ? 1 : 0;
    return true;
}

bool setOption(std::map<std::string, std::string>& options, HostCall& call) {
    std::string key = text(call, 1);
    if (key.empty() || key.size() > 128 || str(call, 2).size() > 65536) {
        return fail(call, "an option has a key of 1 to 128 bytes and a value of at most 64 KiB");
    }
    if (options.size() >= 1024 && !options.contains(key)) {
        return fail(call, "too many options");
    }
    options[key] = text(call, 2);
    return true;
}

bool projectOption(HostCall& call, void* user) { return setOption(project(call, user).options, call); }

TaskSpec* taskOf(HostCall& call, void* user) {
    auto& tasks = project(call, user).tasks;
    std::uint64_t h = call.args[0].u;
    return h == 0 || h > tasks.size() ? nullptr : &tasks[h - 1];
}

bool noTask(HostCall& call) { return fail(call, "not a task of this project"); }

bool taskName(HostCall& call, void* user) {
    TaskSpec* t = taskOf(call, user);
    return t != nullptr ? returnStr(call, t->name) : noTask(call);
}

bool taskDescription(HostCall& call, void* user) {
    TaskSpec* t = taskOf(call, user);
    return t != nullptr ? returnStr(call, t->description) : noTask(call);
}

bool taskGroup(HostCall& call, void* user) {
    TaskSpec* t = taskOf(call, user);
    return t != nullptr ? returnStr(call, t->group) : noTask(call);
}

bool taskSetDescription(HostCall& call, void* user) {
    TaskSpec* t = taskOf(call, user);
    if (t == nullptr) {
        return noTask(call);
    }
    t->description = text(call, 1);
    return true;
}

bool taskSetGroup(HostCall& call, void* user) {
    TaskSpec* t = taskOf(call, user);
    if (t == nullptr) {
        return noTask(call);
    }
    t->group = text(call, 1);
    return true;
}

bool taskDependsOn(HostCall& call, void* user) {
    std::string ref = text(call, 1);
    if (!validTaskRef(ref)) {
        return fail(call, std::format("'{}' is not a task like build or :core:build", ref));
    }
    TaskSpec* found = taskOf(call, user);
    if (found == nullptr) {
        return noTask(call);
    }
    TaskSpec& t = *found;
    if (std::find(t.dependsOn.begin(), t.dependsOn.end(), ref) == t.dependsOn.end()) {
        t.dependsOn.push_back(std::move(ref));
    }
    return true;
}

bool taskAction(HostCall& call, void* user) {
    TaskSpec* found = taskOf(call, user);
    if (found == nullptr) {
        return noTask(call);
    }
    TaskSpec& t = *found;
    std::string plugin = text(call, 1);
    std::string function = text(call, 2);
    if (std::find(std::begin(kBuiltinTasks), std::end(kBuiltinTasks), t.name) != std::end(kBuiltinTasks)) {
        return fail(call, std::format("'{}' is built in; give the plugin's task another name", t.name));
    }
    const auto& plugins = project(call, user).plugins;
    if (std::find(plugins.begin(), plugins.end(), plugin) == plugins.end()) {
        return fail(call, std::format("build plugin {} is not applied to this project (applyPlugin)", plugin));
    }
    if (!validIdentifier(function)) {
        return fail(call, std::format("'{}' is not a function name", function));
    }
    t.plugin = std::move(plugin);
    t.action = std::move(function);
    return true;
}

bool taskOption(HostCall& call, void* user) {
    TaskSpec* t = taskOf(call, user);
    return t != nullptr ? setOption(t->options, call) : noTask(call);
}

bool noProject(HostCall& call, void*) { return fail(call, "settings.qz has no project"); }

} // namespace

void bindProject(quartz::vm::Sandbox& sb, Context& ctx) {
    struct Entry {
        std::string_view name;
        quartz::vm::HostFn fn;
    };

    static constexpr Entry kEntries[] = {
        {"Project.path", getPath},
        {"Project.name", getName},
        {"Project.setName", setName},
        {"Project.group", getGroup},
        {"Project.setGroup", setGroup},
        {"Project.version", getVersion},
        {"Project.setVersion", setVersion},
        {"Project.description", getDescription},
        {"Project.setDescription", setDescription},
        {"Project.kind", getKind},
        {"Project.setKind", setKind},
        {"Project.entry", getEntry},
        {"Project.setEntry", setEntry},
        {"Project.denyWarnings", getDenyWarnings},
        {"Project.setDenyWarnings", setDenyWarnings},
        {"Project.warning", setRuleLevel},
        {"Project.hostOnly", getHostOnly},
        {"Project.setHostOnly", setHostOnly},
        {"Project.packageName", getPackageName},
        {"Project.sourceDir", sourceDir},
        {"Project.hostInterface", hostInterface},
        {"Project.addRepository", addRepository},
        {"Project.addPublishRepository", addPublishRepository},
        {"Project.addDependency", addDependency},
        {"Project.addExport", addExport},
        {"Project.addCapability", addCapability},
        {"Project.setLimits", setLimits},
        {"Project.task", task},
        {"Project.applyPlugin", applyPlugin},
        {"Project.hasPlugin", hasPlugin},
        {"Project.option", projectOption},
        {"Task.name", taskName},
        {"Task.description", taskDescription},
        {"Task.setDescription", taskSetDescription},
        {"Task.group", taskGroup},
        {"Task.setGroup", taskSetGroup},
        {"Task.dependsOn", taskDependsOn},
        {"Task.action", taskAction},
        {"Task.option", taskOption},
    };
    for (const Entry& e : kEntries) {
        std::string name = std::string(kApiPackage) + std::string(e.name);
        // The prelude's methods on Project call these, so a settings script carries the calls it never makes.
        if (ctx.project != nullptr || called(sb, name)) {
            sb.bind(name, ctx.project != nullptr ? e.fn : noProject, &ctx);
        }
    }
}

} // namespace qiln::host
