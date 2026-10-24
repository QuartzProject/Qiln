#include "cli/Commands.hpp"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <format>

namespace {

using qiln::cli::Args;

int usage(int code) {
    std::fprintf(
        code == 0 ? stdout : stderr,
        "usage: qiln <task>... [options]          run tasks: check, test, build, run, publish, clean, or a\n"
        "                                         plugin's; ':core:test' names the project\n"
        "       qiln init [dir] [--kind application|library|plugin|build-plugin] [--group org.example]\n"
        "       qiln tasks | deps [--update]\n"
        "       qiln keygen [file]                a signing key (default ~/.qiln/signing.key)\n"
        "       qiln version | help\n"
        "options: -p <dir>  the project directory (default .)\n"
        "         -P key=value  a property build scripts read with p.property(\"key\")\n"
        "         --filter <text> (test)  --to <dir or URL> (publish)  --sign <seed file> (publish)\n"
        "         -j <n>  tasks at once (default: one per hardware thread)  --rerun  ignore up-to-date checks\n"
        "         --deny-warnings  --update  --locked  --allow-key-change  --no-cache  -q\n"
        "         --allow-insecure-localhost  plain http:// to 127.0.0.1, [::1] or localhost (test servers)\n");
    return code;
}

std::optional<Args> parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        bool value = i + 1 < argc;
        if ((arg == "-p" || arg == "--project-dir") && value) {
            a.dir = argv[++i];
        } else if (arg == "-P" && value) {
            std::string_view kv = argv[++i];
            std::size_t eq = kv.find('=');
            if (eq == std::string_view::npos || eq == 0) {
                std::fprintf(stderr, "-P takes key=value\n");
                return std::nullopt;
            }
            a.options.properties[std::string(kv.substr(0, eq))] = std::string(kv.substr(eq + 1));
        } else if (arg == "--filter" && value) {
            a.run.testFilter = argv[++i];
        } else if (arg == "--to" && value) {
            a.run.publishTo = argv[++i];
        } else if (arg == "--sign" && value) {
            a.run.signingKey = argv[++i];
        } else if (arg == "--kind" && value) {
            a.kind = argv[++i];
        } else if (arg == "--group" && value) {
            a.group = argv[++i];
        } else if (arg == "--deny-warnings") {
            a.run.denyWarnings = true;
        } else if (arg == "--update") {
            a.options.update = true;
        } else if (arg == "--locked") {
            a.options.locked = true;
        } else if (arg == "--allow-key-change") {
            a.options.allowKeyChange = true;
        } else if (((arg == "-j" || arg == "--jobs") && value) || (arg.starts_with("-j") && arg.size() > 2)) {
            std::string_view n = arg.size() > 2 && arg[1] == 'j' ? arg.substr(2) : std::string_view(argv[++i]);
            std::size_t jobs = 0;
            auto r = std::from_chars(n.data(), n.data() + n.size(), jobs);
            if (r.ec != std::errc{} || r.ptr != n.data() + n.size() || jobs == 0 || jobs > 256) {
                std::fprintf(stderr, "-j takes a number of tasks from 1 to 256\n");
                return std::nullopt;
            }
            a.run.jobs = jobs;
        } else if (arg == "--rerun") {
            a.run.rerun = true;
        } else if (arg == "--allow-insecure-localhost") {
            a.options.allowInsecureLocalhost = true;
        } else if (arg == "--no-cache") {
            a.options.noCache = true;
        } else if (arg == "-q" || arg == "--quiet") {
            a.options.quiet = true;
        } else if (arg.starts_with("-") && arg != "-") {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return std::nullopt;
        } else if (a.command.empty()) {
            a.command = arg;
        } else {
            a.positional.emplace_back(arg);
        }
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    auto args = parse(argc, argv);
    if (!args) {
        return usage(2);
    }
    Args& a = *args;
    if (a.command.empty() || a.command == "help") {
        return usage(a.command.empty() ? 2 : 0);
    }
    if (a.command == "version") {
        std::printf("qiln %s (quartz %s)\n", QILN_VERSION, QILN_QUARTZ_VERSION);
        return 0;
    }
    if (a.command == "init") {
        return qiln::cli::init(a);
    }
    if (a.command == "keygen") {
        return qiln::cli::keygen(a);
    }
    auto ws = qiln::loadWorkspace(a.dir, a.options);
    if (!ws) {
        std::fprintf(stderr, "%s\n", ws.error().c_str());
        return 1;
    }
    const qiln::ProjectModel* current = qiln::currentProject(**ws, a.dir);
    if (a.command == "tasks") {
        return qiln::cli::tasks(**ws, current);
    }
    if (a.command == "deps") {
        return qiln::cli::deps(**ws, current);
    }
    std::vector<std::string> requests = {a.command};
    requests.insert(requests.end(), a.positional.begin(), a.positional.end());
    qiln::TaskRunner runner(**ws, a.run);
    auto code = runner.run(requests, current);
    if (!code) {
        std::fprintf(stderr, "%s\n", code.error().c_str());
        return 1;
    }
    return *code;
}
