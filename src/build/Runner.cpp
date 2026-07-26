#include "build/Runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>

namespace qiln {

namespace {

using quartz::vm::HostCall;
using quartz::vm::Sandbox;

std::string_view strArg(const HostCall& call, std::size_t i) {
    return static_cast<const quartz::vm::Str*>(call.args[i].p)->view();
}

bool printLine(HostCall& call, void* user) {
    std::string_view s = strArg(call, 0);
    if (user != nullptr) {
        auto* out = static_cast<std::string*>(user);
        out->append(s).push_back('\n');
        return true;
    }
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    return true;
}

bool ticks(HostCall& call, void*) {
    call.result.u = call.args[0].u;
    return true;
}

// The runner grants the log capability by sending the standard sink to stderr.
bool logLine(HostCall& call, void*) {
    static constexpr const char* kLevels[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    std::string line = std::string("[") + kLevels[std::clamp<std::int64_t>(call.args[0].i, 0, 4)] + "] ";
    line.append(strArg(call, 1)).append(": ").append(strArg(call, 2));
    const auto* fields = static_cast<const quartz::vm::List*>(call.args[3].p);
    for (std::uint32_t i = 0; fields != nullptr && i + 1 < fields->len; i += 2) {
        line.append(" ")
            .append(static_cast<const quartz::vm::Str*>(fields->data[i].p)->view())
            .append("=")
            .append(static_cast<const quartz::vm::Str*>(fields->data[i + 1].p)->view());
    }
    std::fprintf(stderr, "%s\n", line.c_str());
    return true;
}

quartz::vm::Limits limitsOf(const Limits& l) {
    quartz::vm::Limits out;
    out.fuelPerCall = l.fuel;
    out.memoryBytes = static_cast<std::size_t>(l.memoryMiB) << 20;
    out.maxDepth = static_cast<std::uint32_t>(l.maxDepth);
    return out;
}

// Host functions of other hosts' interfaces exist in a test only to be called by mistake.
bool unavailable(HostCall& call, void* user) {
    call.error = std::format("host function '{}' is not available under qiln", *static_cast<const std::string*>(user));
    return false;
}

void bindRunner(Sandbox& sb, std::string* captured) {
    for (const quartz::ir::HostImport& imp : sb.program().module->imports) {
        sb.bind(imp.name, unavailable, const_cast<std::string*>(&imp.name));
    }
    sb.bind("print", printLine, captured);
    sb.bind("int.ticks", ticks);
    sb.bind("org.quartzlang.log.emit", logLine);
}

bool importsFrom(const quartz::vm::Program& program, const std::string& package) {
    std::string prefix = package + ".";
    return std::any_of(program.module->imports.begin(), program.module->imports.end(),
                       [&](const quartz::ir::HostImport& i) { return i.name.starts_with(prefix); });
}

// Sandboxes for the host plugins a program links to, each linked to the plugins it imports from.
class PluginHost {
public:
    std::expected<void, std::string> start(const std::vector<RuntimePlugin>& plugins, const Limits& limits,
                                           std::string* captured) {
        for (const RuntimePlugin& p : plugins) {
            auto sb = std::make_unique<Sandbox>(p.program, limitsOf(limits));
            bindRunner(*sb, captured);
            if (auto l = link(*sb); !l) {
                return std::unexpected(std::format("{}: {}", p.name, l.error()));
            }
            if (auto s = sb->start(); !s) {
                return std::unexpected(std::format("{}: {}", p.name, s.error().describe()));
            }
            names_.push_back(p.name);
            sandboxes_.push_back(std::move(sb));
        }
        return {};
    }

    std::expected<void, std::string> link(Sandbox& client) {
        for (std::size_t i = 0; i < names_.size(); ++i) {
            if (importsFrom(client.program(), names_[i])) {
                if (auto l = client.link(names_[i], *sandboxes_[i]); !l) {
                    return std::unexpected(l.error());
                }
            }
        }
        return {};
    }

private:
    std::vector<std::string> names_;
    std::vector<std::unique_ptr<Sandbox>> sandboxes_;
};

} // namespace

std::expected<bool, std::string> runTests(const Compiled& compiled, const Limits& limits, std::string_view filter) {
    const auto& program = compiled.program;
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const quartz::ir::Annotation& a : program->module->annotations) {
        if (a.name != "test") {
            continue;
        }
        const std::string& name = program->functions[a.function].name;
        if (!filter.empty() && name.find(filter) == std::string::npos) {
            continue;
        }
        std::string printed;
        PluginHost host;
        Sandbox sb(program, limitsOf(limits));
        bindRunner(sb, &printed);
        if (auto l = host.start(compiled.plugins, limits, &printed).and_then([&] { return host.link(sb); }); !l) {
            return std::unexpected(l.error());
        }
        auto start = std::chrono::steady_clock::now();
        auto started = sb.start();
        auto r = started ? sb.call(a.function, {})
                         : std::expected<quartz::vm::Slot, quartz::vm::Trap>(std::unexpected(started.error()));
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (r) {
            ++passed;
            std::printf("  ok      %s (%.1f ms)\n", name.c_str(), ms);
            continue;
        }
        ++failed;
        std::printf("  FAILED  %s: %s\n", name.c_str(), r.error().describe().c_str());
        if (!printed.empty()) {
            std::printf("%s", printed.c_str());
        }
    }
    if (passed + failed == 0) {
        std::printf("  no tests%s\n", filter.empty() ? "" : " match the filter");
        return true;
    }
    std::printf("  %zu passed, %zu failed\n", passed, failed);
    return failed == 0;
}

std::expected<int, std::string> runEntry(const Compiled& compiled, const Limits& limits, const std::string& entry) {
    const auto& program = compiled.program;
    std::uint32_t fn = program->findFunction(entry);
    if (fn == ~0U) {
        return std::unexpected(std::format("no function '{}' to run (set p.entry)", entry));
    }
    if (!program->functions[fn].params.empty()) {
        return std::unexpected(std::format("'{}' takes parameters; the entry point takes none", entry));
    }
    PluginHost host;
    if (auto l = host.start(compiled.plugins, limits, nullptr); !l) {
        return std::unexpected(l.error());
    }
    Sandbox sb(program, limitsOf(limits));
    bindRunner(sb, nullptr);
    if (auto l = host.link(sb); !l) {
        return std::unexpected(l.error());
    }
    if (auto s = sb.start(); !s) {
        return std::unexpected(s.error().describe());
    }
    auto t = sb.spawn(fn, {});
    while (t && t->kind != quartz::vm::TaskOutcome::Kind::Done) {
        t = sb.resume(t->task);
    }
    if (!t) {
        return std::unexpected(t.error().describe());
    }
    std::fflush(stdout);
    if (program->functions[fn].result == quartz::ir::kI64) {
        return static_cast<int>(t->value.i & 0xFF);
    }
    return 0;
}

} // namespace qiln
