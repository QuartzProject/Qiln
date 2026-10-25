#include "build/Stamp.hpp"
#include "repo/Archive.hpp"
#include "repo/Credentials.hpp"
#include "repo/Meta.hpp"
#include "repo/Metadata.hpp"
#include "resolve/Lockfile.hpp"
#include "script/Edit.hpp"
#include "workspace/ConfigCache.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// The first byte picks the reader (or the build.qz editor); the rest is its input.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    std::span<const std::uint8_t> bytes(data + 1, size - 1);
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    switch (data[0] % 8) {
    case 0:
        (void)qiln::parseMeta(text, "org.example.text", {1, 2, 0});
        break;
    case 1:
        (void)qiln::parseLock(text);
        break;
    case 2:
        (void)qiln::parseMetadata(text, "org.example.text");
        break;
    case 3:
        (void)qiln::unpackSources(bytes);
        break;
    case 4:
        (void)qiln::parseCredentials(text);
        break;
    case 5:
        (void)qiln::decodeStamp(bytes);
        break;
    case 6:
        (void)qiln::edit::addDependency(text, {"org.example.text:^1.2", (size & 1) != 0});
        (void)qiln::edit::removeDependency(text, (size & 2) != 0 ? ":core" : "org.example.text");
        break;
    default:
        (void)qiln::decodeConfiguration(bytes);
    }
    return 0;
}
