#include "build/Console.hpp"
#include "build/Tasks.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <format>
#include <thread>

namespace qiln {

namespace {

// The tasks to run in dependency order, and for each the tasks that must finish before it starts.
struct Plan {
    std::map<std::string, std::vector<TaskRef>> tasks; // every task by project; moving the map keeps its nodes
    std::vector<const TaskRef*> order;
    std::vector<std::vector<std::size_t>> after; // dependents of each task
    std::vector<std::size_t> waiting;            // how many tasks each one waits for
};

void addEdge(Plan& plan, std::set<std::pair<std::size_t, std::size_t>>& seen, std::size_t from, std::size_t to) {
    if (from != to && seen.insert({from, to}).second) {
        plan.after[from].push_back(to);
        ++plan.waiting[to];
    }
}

std::expected<Plan, std::string> planOf(const Workspace& ws, const std::vector<std::string>& requests,
                                        const ProjectModel* current) {
    Plan plan;
    auto& all = plan.tasks;
    for (const ProjectModel& p : ws.model.projects) {
        all[p.path] = tasksOf(p);
    }
    auto find = [&](const std::string& path, const std::string& name) -> const TaskRef* {
        auto it = all.find(path);
        if (it == all.end()) {
            return nullptr;
        }
        for (const TaskRef& t : it->second) {
            if (t.name == name) {
                return &t;
            }
        }
        return nullptr;
    };
    std::map<const TaskRef*, std::size_t> index;
    std::map<const TaskRef*, std::vector<const TaskRef*>> needs;
    std::vector<const TaskRef*> stack;
    auto visit = [&](auto& self, const TaskRef* t) -> std::expected<void, std::string> {
        if (index.contains(t)) {
            return {};
        }
        if (std::find(stack.begin(), stack.end(), t) != stack.end()) {
            std::string cycle;
            for (const TaskRef* s : stack) {
                cycle += qualifiedName(*s->project, s->name) + " -> ";
            }
            return std::unexpected("tasks depend on each other: " + cycle + qualifiedName(*t->project, t->name));
        }
        stack.push_back(t);
        for (const std::string& dep : t->dependsOn) {
            std::string path = t->project->path;
            std::string name = dep;
            if (dep.starts_with(':')) {
                std::size_t last = dep.rfind(':');
                path = last == 0 ? ":" : dep.substr(0, last);
                name = dep.substr(last + 1);
            }
            const TaskRef* target = find(path, name);
            if (target == nullptr) {
                return std::unexpected(
                    std::format("{} depends on {}, which does not exist", qualifiedName(*t->project, t->name), dep));
            }
            if (auto r = self(self, target); !r) {
                return r;
            }
            needs[t].push_back(target);
        }
        stack.pop_back();
        index[t] = plan.order.size();
        plan.order.push_back(t);
        return {};
    };
    for (const std::string& request : requests) {
        std::vector<const TaskRef*> roots;
        if (request.starts_with(':')) {
            std::size_t last = request.rfind(':');
            std::string path = last == 0 ? ":" : request.substr(0, last);
            if (const TaskRef* t = find(path, request.substr(last + 1))) {
                roots.push_back(t);
            }
        } else {
            for (const ProjectModel& p : ws.model.projects) {
                if (current == nullptr || current == &p) {
                    if (const TaskRef* t = find(p.path, request)) {
                        roots.push_back(t);
                    }
                }
            }
        }
        if (roots.empty()) {
            return std::unexpected(std::format("no task '{}' here; 'qiln tasks' lists them", request));
        }
        for (const TaskRef* t : roots) {
            if (auto r = visit(visit, t); !r) {
                return std::unexpected(r.error());
            }
        }
    }
    std::size_t n = plan.order.size();
    plan.after.resize(n);
    plan.waiting.resize(n);
    std::set<std::pair<std::size_t, std::size_t>> seen;
    for (std::size_t i = 0; i < n; ++i) {
        for (const TaskRef* dep : needs[plan.order[i]]) {
            addEdge(plan, seen, index[dep], i);
        }
    }
    // `clean` keeps its place among its project's tasks, and `run` owns the terminal while it runs.
    for (std::size_t i = 0; i < n; ++i) {
        const TaskRef& t = *plan.order[i];
        bool clean = t.builtin && t.name == "clean";
        bool run = t.builtin && t.name == "run";
        for (std::size_t j = 0; (clean || run) && j < n; ++j) {
            if (run || plan.order[j]->project == t.project) {
                addEdge(plan, seen, std::min(i, j), std::max(i, j));
            }
        }
    }
    return plan;
}

} // namespace

std::expected<int, std::string> TaskRunner::run(const std::vector<std::string>& requests, const ProjectModel* current) {
    auto planned = planOf(ws_, requests, current);
    if (!planned) {
        return std::unexpected(planned.error());
    }
    Plan& plan = *planned;
    if (auto p = prepare(plan.order); !p) {
        return std::unexpected(p.error());
    }
    std::size_t n = plan.order.size();
    std::size_t jobs =
        options_.jobs != 0 ? options_.jobs : std::max<std::size_t>(1, std::thread::hardware_concurrency());
    jobs = std::clamp<std::size_t>(jobs, 1, std::max<std::size_t>(n, 1));

    std::mutex mutex;
    std::condition_variable wake;
    std::set<std::size_t> ready;
    for (std::size_t i = 0; i < n; ++i) {
        if (plan.waiting[i] == 0) {
            ready.insert(i);
        }
    }
    std::vector<TaskLog> logs(n);
    std::vector<std::optional<std::expected<Outcome, std::string>>> results(n);
    std::vector<bool> headed(n, false);
    std::size_t printed = 0;
    std::size_t running = 0;
    bool failed = false;
    auto header = [&](std::size_t i, bool upToDate) {
        std::printf("> %s%s\n", qualifiedName(*plan.order[i]->project, plan.order[i]->name).c_str(),
                    upToDate ? " UP-TO-DATE" : "");
        std::fflush(stdout);
        headed[i] = true;
    };
    // Output appears in the planned order whatever order the tasks finish in.
    auto flush = [&] {
        for (; printed < n && results[printed]; ++printed) {
            if (!headed[printed]) {
                header(printed, *results[printed] && (*results[printed])->upToDate);
            }
            logs[printed].replay();
        }
    };
    auto work = [&] {
        std::unique_lock lock(mutex);
        while (true) {
            wake.wait(lock, [&] { return !ready.empty() || running == 0; });
            if (ready.empty()) {
                return;
            }
            std::size_t i = *ready.begin();
            ready.erase(ready.begin());
            ++running;
            bool streamed = plan.order[i]->builtin && plan.order[i]->name == "run";
            if (streamed) {
                flush();
                header(i, false);
            }
            lock.unlock();
            std::expected<Outcome, std::string> r = Outcome{};
            {
                LogScope scope(streamed ? nullptr : &logs[i]);
                r = execute(*plan.order[i]);
            }
            lock.lock();
            --running;
            failed = failed || !r;
            results[i] = std::move(r);
            if (failed) {
                ready.clear();
            } else {
                for (std::size_t j : plan.after[i]) {
                    if (--plan.waiting[j] == 0) {
                        ready.insert(j);
                    }
                }
            }
            flush();
            wake.notify_all();
        }
    };
    {
        std::vector<std::jthread> workers;
        for (std::size_t w = 1; w < jobs; ++w) {
            workers.emplace_back(work);
        }
        work();
    }
    std::string errors;
    int code = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (i >= printed && results[i]) {
            if (!headed[i]) {
                header(i, *results[i] && (*results[i])->upToDate);
            }
            logs[i].replay();
        }
        if (results[i] && !*results[i]) {
            errors += std::format("{}{} failed: {}", errors.empty() ? "" : "\n",
                                  qualifiedName(*plan.order[i]->project, plan.order[i]->name), results[i]->error());
        } else if (results[i] && code == 0) {
            code = (*results[i])->code;
        }
    }
    if (!errors.empty()) {
        return std::unexpected(errors);
    }
    return code;
}

} // namespace qiln
