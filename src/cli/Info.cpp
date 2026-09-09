#include "cli/Commands.hpp"

#include <cstdio>
#include <format>
#include <set>

namespace qiln::cli {

namespace {

std::vector<const ProjectModel*> scope(const Workspace& ws, const ProjectModel* current) {
    std::vector<const ProjectModel*> out;
    for (const ProjectModel& p : ws.model.projects) {
        if (current == nullptr || current == &p) {
            out.push_back(&p);
        }
    }
    return out;
}

std::string header(const ProjectModel& p) {
    return std::format("{} ({} {}, {})", p.path, p.packageName(), p.version, kindName(p.kind));
}

void printPackage(const LockGraph& g, const std::string& name, const std::string& range, const std::string& indent,
                  bool last, std::set<std::string>& shown) {
    const Locked* l = g.find(name);
    std::string version = l == nullptr ? "not locked" : quartz::formatVersion(l->version);
    bool repeat = l != nullptr && !l->dependencies.empty() && shown.contains(name);
    std::printf("%s%s %s %s -> %s%s%s\n", indent.c_str(), last ? "└──" : "├──", name.c_str(), range.c_str(),
                version.c_str(),
                l != nullptr && l->kind != ArtifactKind::Library ? std::format(" ({})", kindName(l->kind)).c_str() : "",
                repeat ? " (*)" : "");
    if (l == nullptr || repeat) {
        return;
    }
    shown.insert(name);
    std::string next = indent + (last ? "    " : "│   ");
    for (std::size_t i = 0; i < l->dependencies.size(); ++i) {
        const Dependency& d = l->dependencies[i];
        printPackage(g, d.name, d.range, next, i + 1 == l->dependencies.size(), shown);
    }
}

} // namespace

int tasks(Workspace& ws, const ProjectModel* current) {
    for (const ProjectModel* p : scope(ws, current)) {
        std::printf("%s\n", header(*p).c_str());
        std::vector<TaskRef> list = tasksOf(*p);
        std::size_t width = 0;
        for (const TaskRef& t : list) {
            width = std::max(width, t.name.size());
        }
        std::string group;
        for (const std::string& g : {"build", "verification", "application", "publishing"}) {
            for (const TaskRef& t : list) {
                if (t.group == g) {
                    std::printf("  %-*s  %-12s %s\n", static_cast<int>(width), t.name.c_str(), t.group.c_str(),
                                t.description.c_str());
                }
            }
        }
        for (const TaskRef& t : list) {
            if (t.group != "build" && t.group != "verification" && t.group != "application" &&
                t.group != "publishing") {
                std::string from = t.spec != nullptr && !t.spec->plugin.empty() ? " [" + t.spec->plugin + "]" : "";
                std::printf("  %-*s  %-12s %s%s\n", static_cast<int>(width), t.name.c_str(), t.group.c_str(),
                            t.description.c_str(), from.c_str());
            }
        }
    }
    return 0;
}

int deps(Workspace& ws, const ProjectModel* current) {
    auto graph = ws.graph(false);
    if (!graph) {
        std::fprintf(stderr, "%s\n", graph.error().c_str());
        return 1;
    }
    for (const ProjectModel* p : scope(ws, current)) {
        std::printf("%s\n", header(*p).c_str());
        std::set<std::string> shown;
        for (std::size_t i = 0; i < p->dependencies.size(); ++i) {
            const DependencySpec& d = p->dependencies[i];
            bool last = i + 1 == p->dependencies.size();
            std::string tag = d.configuration == "test" ? " [test]" : "";
            if (!d.project.empty()) {
                std::printf("%s %s (project)%s\n", last ? "└──" : "├──", d.project.c_str(), tag.c_str());
                continue;
            }
            printPackage(**graph, d.name, d.range + tag, "", last, shown);
        }
        if (p->dependencies.empty()) {
            std::printf("  (no dependencies)\n");
        }
    }
    if (!ws.model.plugins.empty()) {
        auto plugins = ws.plugins();
        if (!plugins) {
            std::fprintf(stderr, "%s\n", plugins.error().c_str());
            return 1;
        }
        std::printf("build plugins\n");
        for (std::size_t i = 0; i < (*plugins)->size(); ++i) {
            const LoadedPlugin& lp = (**plugins)[i];
            std::printf("%s %s %s (%s)\n", i + 1 == (*plugins)->size() ? "└──" : "├──", lp.name.c_str(),
                        lp.version.c_str(), lp.origin.c_str());
        }
    }
    return 0;
}

} // namespace qiln::cli
