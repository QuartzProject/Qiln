#include "resolve/Resolver.hpp"

#include "repo/Publish.hpp"
#include "repo/Repositories.hpp"
#include "support/Files.hpp"

#include <doctest/doctest.h>

#include <format>

namespace qiln {

namespace {

// A repository directory written by the same code `qiln publish` uses.
struct Fixture {
    fs::path dir = fs::path(QILN_TEST_WORK) / "repo";
    Seed seed{};
    Seed other{};

    Fixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
        seed.fill(7);
        other.fill(9);
        add("org.example.mathx", {1, 0, 0}, {});
        add("org.example.mathx", {1, 4, 3}, {});
        add("org.example.mathx", {2, 0, 0}, {});
        add("org.example.text", {1, 2, 0}, {{"org.example.mathx", ">=1.1, <2"}});
        add("org.example.text", {1, 3, 0}, {{"org.example.mathx", "^2.0"}});
        add("org.example.app", {1, 0, 0}, {{"org.example.text", "^1.2"}}, other);
        add("org.example.app", {1, 1, 0}, {{"org.example.text", "^1.2"}});
    }

    void add(const std::string& name, quartz::Version v, std::vector<Dependency> deps) { add(name, v, deps, seed); }

    void add(const std::string& name, quartz::Version v, std::vector<Dependency> deps, const Seed& key) {
        Publication pub;
        pub.meta.name = name;
        pub.meta.version = v;
        pub.meta.quartz = "0.1.0";
        pub.meta.dependencies = std::move(deps);
        pub.meta.archive = std::format("{}-{}.qzs", coordinatesOf(name)->artifact, quartz::formatVersion(v));
        std::string body = std::format("{} {}", name, quartz::formatVersion(v));
        pub.artifact.assign(body.begin(), body.end());
        Location loc;
        loc.local = dir;
        loc.display = "fixture";
        REQUIRE(publish(loc, pub, key));
    }
};

std::string versions(const LockGraph& g) {
    std::string out;
    for (const Locked& p : g.packages) {
        out += std::format("{}{}={}", out.empty() ? "" : " ", coordinatesOf(p.name)->artifact,
                           quartz::formatVersion(p.version));
    }
    return out;
}

} // namespace

TEST_CASE("the resolver picks the highest version every range accepts, from a Maven layout repository") {
    Fixture f;
    Location loc;
    loc.local = f.dir;
    Repositories repos({loc});
    PackageSource source{[&](const std::string& n) { return repos.versions(n); },
                         [&](const std::string& n, const quartz::Version& v) { return repos.meta(n, v); }};

    // text 1.3.0 wants mathx ^2.0, which the root's range on mathx rules out, so text settles on 1.2.0.
    auto both = resolve({{"org.example.text", "^1.2", "root"}, {"org.example.mathx", "^1.0", "root"}}, source);
    REQUIRE(both);
    CHECK(versions(*both) == "mathx=1.4.3 text=1.2.0");

    auto alone = resolve({{"org.example.text", "^1.2", "root"}}, source);
    REQUIRE(alone);
    CHECK(versions(*alone) == "mathx=2.0.0 text=1.3.0");

    LockGraph old = *both;
    auto kept = resolve({{"org.example.text", "^1.2", "root"}}, source, {&old, false, false});
    REQUIRE(kept);
    CHECK(versions(*kept) == "mathx=1.4.3 text=1.2.0");

    auto conflict = resolve({{"org.example.mathx", "^1.0", "a"}, {"org.example.mathx", "^2.0", "b"}}, source);
    REQUIRE_FALSE(conflict);
    CHECK(conflict.error().find("no version of org.example.mathx satisfies ^1.0 (from a), ^2.0 (from b)") !=
          std::string::npos);

    // The oldest app was signed by another key: trust on first use refuses the newer one.
    auto changed = resolve({{"org.example.app", "^1.0", "root"}}, source);
    REQUIRE_FALSE(changed);
    CHECK(changed.error().find("--allow-key-change") != std::string::npos);
    CHECK(resolve({{"org.example.app", "^1.0", "root"}}, source, {nullptr, false, true}));

    auto missing = resolve({{"org.example.nothing", "^1.0", "root"}}, source);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().find("not in any repository") != std::string::npos);

    std::string meta = readText(f.dir / "org/example/text/1.2.0/text-1.2.0.qzmeta").value_or("");
    CHECK_FALSE(parseMeta(meta + "exports = []\n", "org.example.text", {1, 2, 0}));
    std::string tampered = meta;
    tampered.replace(tampered.find(">=1.1"), 5, ">=1.0");
    CHECK_FALSE(parseMeta(tampered, "org.example.text", {1, 2, 0}));
}

} // namespace qiln
