#include "script/Host.hpp"
#include "support/Files.hpp"

#include <algorithm>
#include <cstdio>
#include <format>

namespace qiln::host {

namespace {

using quartz::vm::HostCall;

bool lower(char c) { return c >= 'a' && c <= 'z'; }

bool digit(char c) { return c >= '0' && c <= '9'; }

bool settingsName(HostCall& call, void* user) { return returnStr(call, context(user).workspace->name); }

bool settingsSetName(HostCall& call, void* user) {
    if (!validName(str(call, 1))) {
        return fail(call,
                    std::format("'{}' is not a name like 'shop' (lower case letters, digits and _)", str(call, 1)));
    }
    context(user).workspace->name = std::string(str(call, 1));
    return true;
}

bool settingsInclude(HostCall& call, void* user) {
    std::string path(str(call, 1));
    if (!safeRelativePath(path)) {
        return fail(call, std::format("'{}' is not a relative directory like 'core' or 'libs/text'", path));
    }
    auto& includes = context(user).workspace->includes;
    if (std::find(includes.begin(), includes.end(), path) == includes.end()) {
        includes.push_back(std::move(path));
    }
    return true;
}

bool settingsIncludePlugin(HostCall& call, void* user) {
    std::string path(str(call, 1));
    std::string_view rest = path;
    while (rest.starts_with("../")) {
        rest.remove_prefix(3);
    }
    if (!safeRelativePath(rest)) {
        return fail(call, std::format("'{}' is not a relative directory like plugins/bundle or ../bundle", path));
    }
    auto& plugins = context(user).workspace->plugins;
    if (std::none_of(plugins.begin(), plugins.end(), [&](const PluginRequest& p) { return p.dir == path; })) {
        plugins.push_back({"", "", std::move(path)});
    }
    return true;
}

bool settingsPluginRepository(HostCall& call, void* user) {
    if (str(call, 2).empty()) {
        return fail(call, "a repository needs a directory or an https:// URL");
    }
    context(user).workspace->pluginRepositories.push_back({std::string(str(call, 1)), std::string(str(call, 2))});
    return true;
}

bool settingsPlugin(HostCall& call, void* user) {
    auto n = parseNotation(str(call, 1));
    if (!n) {
        return fail(call, n.error());
    }
    auto& plugins = context(user).workspace->plugins;
    if (std::any_of(plugins.begin(), plugins.end(), [&](const PluginRequest& p) { return p.name == n->first; })) {
        return fail(call, std::format("plugin {} is requested twice", n->first));
    }
    plugins.push_back({n->first, n->second, ""});
    return true;
}

bool property(HostCall& call, void* user) {
    const Properties& props = *context(user).properties;
    auto it = props.find(std::string(str(call, 1)));
    if (it == props.end()) {
        call.result.p = nullptr;
        return true;
    }
    return returnStr(call, it->second);
}

bool noSettings(HostCall& call, void*) { return fail(call, "only settings.qz configures the workspace"); }

bool log(HostCall& call, void* user) {
    std::string_view text = str(call, 0);
    std::fprintf(stderr, "[%s] %.*s\n", context(user).scriptName.c_str(), static_cast<int>(text.size()), text.data());
    return true;
}

} // namespace

bool validName(std::string_view s) {
    return !s.empty() && s.size() <= 64 && lower(s[0]) &&
           std::all_of(s.begin(), s.end(), [](char c) { return lower(c) || digit(c) || c == '_'; });
}

bool validIdentifier(std::string_view s) {
    auto start = [](char c) { return lower(c) || (c >= 'A' && c <= 'Z') || c == '_'; };
    return !s.empty() && s.size() <= 128 && start(s[0]) &&
           std::all_of(s.begin(), s.end(), [&](char c) { return start(c) || digit(c); });
}

bool validTaskName(std::string_view s) {
    return !s.empty() && s.size() <= 64 && (lower(s[0]) || (s[0] >= 'A' && s[0] <= 'Z')) &&
           std::all_of(s.begin(), s.end(),
                       [](char c) { return lower(c) || (c >= 'A' && c <= 'Z') || digit(c) || c == '_' || c == '-'; });
}

bool validTaskRef(std::string_view s) {
    if (!s.starts_with(':')) {
        return validTaskName(s);
    }
    std::size_t last = s.rfind(':');
    std::string_view project = s.substr(1, last == 0 ? 0 : last - 1);
    if (last != 0 && !project.empty()) {
        std::string dir(project);
        std::replace(dir.begin(), dir.end(), ':', '/');
        if (!safeRelativePath(dir)) {
            return false;
        }
    }
    return validTaskName(s.substr(last + 1));
}

std::expected<std::pair<std::string, std::string>, std::string> parseNotation(std::string_view notation) {
    std::size_t colon = notation.find(':');
    if (colon == std::string_view::npos) {
        return std::unexpected(
            std::format("'{}' needs a version range after a colon, like org.example.text:^1.2", notation));
    }
    std::string name(notation.substr(0, colon));
    std::string range(notation.substr(colon + 1));
    while (!range.empty() && range.front() == ' ') {
        range.erase(range.begin());
    }
    if (!coordinatesOf(name)) {
        return std::unexpected(std::format("'{}' is not a package name like org.example.text", name));
    }
    if (range.empty() || range.size() > 128) {
        return std::unexpected(std::format("'{}' needs a version range like ^1.2, ~1.4 or >=1.0, <2", notation));
    }
    if (auto ok = quartz::satisfies(quartz::Version{}, range); !ok) {
        return std::unexpected(std::format("'{}' is not a version range: {}", range, ok.error()));
    }
    return std::pair(std::move(name), std::move(range));
}

void bindCommon(quartz::vm::Sandbox& sb, Context& ctx) { sb.bind("org.quartzlang.qiln.log", log, &ctx); }

void bindSettings(quartz::vm::Sandbox& sb, Context& ctx) {
    auto pick = [&](quartz::vm::HostFn fn) { return ctx.workspace != nullptr ? fn : noSettings; };
    sb.bind("Settings.name", pick(settingsName), &ctx);
    sb.bind("Settings.setName", pick(settingsSetName), &ctx);
    sb.bind("Settings.include", pick(settingsInclude), &ctx);
    sb.bind("Settings.includePlugin", pick(settingsIncludePlugin), &ctx);
    sb.bind("Settings.addPluginRepository", pick(settingsPluginRepository), &ctx);
    sb.bind("Settings.addPlugin", pick(settingsPlugin), &ctx);
    sb.bind("Settings.property", property, &ctx);
    sb.bind("Project.property", property, &ctx);
}

} // namespace qiln::host
