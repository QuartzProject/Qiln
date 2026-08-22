#include "resolve/Lockfile.hpp"

#include <doctest/doctest.h>

namespace qiln {

TEST_CASE("qiln.lock reads back what it writes and knows when it no longer covers the build") {
    Lockfile lock;
    Locked text{"org.example.text", {1, 2, 0}, ArtifactKind::Library, {{"org.example.mathx", ">=1.1, <2"}}, {}, {}};
    Locked mathx{"org.example.mathx", {1, 4, 3}, ArtifactKind::Library, {}, {}, {}};
    Locked plugin{"org.example.bundle", {1, 0, 0}, ArtifactKind::BuildPlugin, {}, {}, {}};
    text.checksum.fill(0xab);
    plugin.key.fill(0x11);
    lock.packages.packages = {mathx, text};
    lock.plugins.packages = {plugin};

    auto again = parseLock(printLock(lock));
    REQUIRE(again);
    CHECK(printLock(*again) == printLock(lock));
    CHECK(again->packages.find("org.example.text")->dependencies.front().range == ">=1.1, <2");
    CHECK(again->plugins.find("org.example.bundle")->kind == ArtifactKind::BuildPlugin);

    CHECK(graphCovers(again->packages, {{"org.example.text", "^1.2", ":app"}}));
    CHECK_FALSE(graphCovers(again->packages, {{"org.example.text", "^1.3", ":app"}}));
    CHECK_FALSE(graphCovers(again->packages, {{"org.example.other", "^1.0", ":app"}}));
    CHECK_FALSE(parseLock("format = 2\n"));
    CHECK_FALSE(parseLock("format = 1\n[[package]]\nname = \"x\"\n"));
}

} // namespace qiln
