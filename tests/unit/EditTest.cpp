#include "script/Edit.hpp"

#include <doctest/doctest.h>

namespace qiln {

namespace {

// The greeter's build.qz from the README.
constexpr std::string_view kReadme = R"(import org.quartzlang.qiln.*
import org.example.bundle.{bundle, Bundle}

fn build(p: Project) {
    p.group = "org.example.shop"
    p.version = "1.0.0"
    p.kind = Kind.Plugin
    p.hostOnly = true
    p.hostInterface("server.qzi")
    p.exports(["onJoin"])
    p.repository(Repository.maven("https://repo.example.org/quartz"))
    p.dependencies([
        project(":core"),
        implementation("org.example.text:^1.2"),
        testImplementation("org.example.check:~0.4"),
    ])
    p.limits(Limits(fuel = 10000000) with { memoryMiB = 16 })
    bundle(p, Bundle(manifest = "plugin.toml"))
    let release = p.task("release")
    release.description = "Everything a release needs"
    release.dependsOn("bundle")
}
)";

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    text.replace(text.find(from), from.size(), to);
    return text;
}

} // namespace

TEST_CASE("qiln add and remove edit the dependencies list of the README example and nothing else") {
    auto added = edit::addDependency(kReadme, {"org.example.json:^2.0", false});
    REQUIRE(added);
    CHECK(added->text == replaced(std::string(kReadme), "        testImplementation(\"org.example.check:~0.4\"),\n",
                                  "        testImplementation(\"org.example.check:~0.4\"),\n"
                                  "        implementation(\"org.example.json:^2.0\"),\n"));

    auto changed = edit::addDependency(kReadme, {"org.example.text:^1.4", true});
    REQUIRE(changed);
    CHECK(changed->text == replaced(std::string(kReadme), "implementation(\"org.example.text:^1.2\")",
                                    "testImplementation(\"org.example.text:^1.4\")"));

    auto removed = edit::removeDependency(kReadme, ":core");
    REQUIRE(removed);
    CHECK(removed->text == replaced(std::string(kReadme), "        project(\":core\"),\n", ""));
}

} // namespace qiln
