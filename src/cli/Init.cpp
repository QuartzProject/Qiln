#include "cli/Commands.hpp"
#include "support/Files.hpp"

#include <cstdio>
#include <format>

namespace qiln::cli {

namespace {

std::string kindConstant(ArtifactKind k) {
    switch (k) {
    case ArtifactKind::Application:
        return "Kind.Application";
    case ArtifactKind::Plugin:
        return "Kind.Plugin";
    case ArtifactKind::BuildPlugin:
        return "Kind.BuildPlugin";
    case ArtifactKind::Library:
        break;
    }
    return "Kind.Library";
}

std::string buildScript(const std::string& group, ArtifactKind kind) {
    return std::format(R"(import org.quartzlang.qiln.*

fn build(p: Project) {{
    p.group = "{}"
    p.version = "0.1.0"
    p.kind = {}
}}
)",
                       group, kindConstant(kind));
}

std::string mainSource(const std::string& package, ArtifactKind kind) {
    std::string body = std::format(R"(package {}

pub fn greet(name: str) -> str {{
    return "Hello, {{name}}!"
}}
)",
                                   package);
    if (kind == ArtifactKind::Application) {
        body += R"(
fn main() {
    print(greet("Quartz"))
}
)";
    }
    return body;
}

std::string testSource(const std::string& package) {
    return std::format(R"(package {}

@test
fn greetsByName() {{
    if greet("Ada") != "Hello, Ada!" {{
        panic("unexpected greeting: {{greet("Ada")}}")
    }}
}}
)",
                       package);
}

std::string pluginApi(const std::string& package) {
    return std::format(R"(package {}

import org.quartzlang.qiln.*

/// Adds a `hello` task that writes a greeting into the build directory.
pub fn hello(p: Project) {{
    p.applyPlugin("{}")
    let t = p.task("hello")
    t.description = "Writes build/hello.txt"
    t.action("{}", "hello")
}}
)",
                       package, package, package);
}

std::string pluginTasks(const std::string& package) {
    return std::format(R"(package {}.tasks

import org.quartzlang.qiln.task.*

pub fn hello(t: TaskContext) -> Result<str, str> {{
    if !t.writeBuildText("hello.txt", "Hello from {{t.projectName}}\n") {{
        return Err("cannot write hello.txt")
    }}
    return Ok("wrote build/hello.txt")
}}
)",
                       package);
}

} // namespace

int init(const Args& a) {
    fs::path dir = a.positional.empty() ? fs::path(".") : fs::path(a.positional.front());
    std::error_code ec;
    if (fs::exists(dir / "build.qz", ec) || fs::exists(dir / "settings.qz", ec)) {
        std::fprintf(stderr, "%s already has a build.qz or settings.qz\n", dir.string().c_str());
        return 2;
    }
    auto kind = parseKind(a.kind == "app" ? "application" : a.kind == "lib" ? "library" : a.kind);
    if (!kind) {
        std::fprintf(stderr, "%s\n", kind.error().c_str());
        return 2;
    }
    fs::path absolute = fs::weakly_canonical(fs::absolute(dir, ec), ec);
    std::string name = defaultProjectName(absolute.filename().string());
    std::string group = a.group.empty() ? "org.example" : a.group;
    if (!coordinatesOf(group + "." + name)) {
        std::fprintf(stderr, "'%s' is not a group like org.example\n", group.c_str());
        return 2;
    }
    std::string package = group + "." + name;
    std::vector<std::pair<fs::path, std::string>> files = {
        {"build.qz", buildScript(group, *kind)},
        {".gitignore", "build/\n.qiln/\n"},
    };
    if (*kind == ArtifactKind::BuildPlugin) {
        files.emplace_back("api/" + name + ".qz", pluginApi(package));
        files.emplace_back("src/tasks.qz", pluginTasks(package));
    } else {
        files.emplace_back("src/main.qz", mainSource(package, *kind));
        files.emplace_back("test/main_test.qz", testSource(package));
    }
    for (const auto& [rel, text] : files) {
        if (auto w = writeAtomically(dir / rel, text); !w) {
            std::fprintf(stderr, "%s\n", w.error().c_str());
            return 2;
        }
    }
    std::printf("created %s %s in %s; try 'qiln test'%s\n", std::string(kindName(*kind)).c_str(), package.c_str(),
                dir.string().c_str(), *kind == ArtifactKind::Application ? " and 'qiln run'" : "");
    return 0;
}

int keygen(const Args& a) {
    fs::path path = a.positional.empty() ? qilnHome() / "signing.key" : fs::path(a.positional.front());
    std::error_code ec;
    if (fs::exists(path, ec)) {
        std::fprintf(stderr, "%s already exists; delete it first to replace the key\n", path.string().c_str());
        return 2;
    }
    Seed seed = randomSeed();
    if (auto w = writeAtomically(path, toHex(seed) + "\n"); !w) {
        std::fprintf(stderr, "%s\n", w.error().c_str());
        return 2;
    }
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, ec);
    std::printf("wrote %s; public key %s\n", path.string().c_str(), toHex(publicKeyOf(seed)).c_str());
    return 0;
}

} // namespace qiln::cli
