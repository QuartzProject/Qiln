#include "repo/Archive.hpp"
#include "repo/Meta.hpp"
#include "repo/Metadata.hpp"
#include "resolve/Lockfile.hpp"
#include "workspace/ConfigCache.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// The first byte picks the reader; the rest is its input.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    std::span<const std::uint8_t> bytes(data + 1, size - 1);
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    switch (data[0] % 5) {
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
    default:
        (void)qiln::decodeConfiguration(bytes);
    }
    return 0;
}
