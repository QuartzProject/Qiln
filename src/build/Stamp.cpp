#include "build/Stamp.hpp"

#include "ir/ByteIo.hpp"

#include <algorithm>

namespace qiln {

namespace {

namespace bin = quartz::ir::bin;

constexpr std::string_view kMagic = "QLNT";
constexpr std::uint64_t kVersion = 1;
constexpr std::uint64_t kMaxEntries = 1u << 16;

void writeList(bin::Writer& w, const std::vector<std::pair<std::string, Hash32>>& list) {
    w.u64(list.size());
    for (const auto& [id, hash] : list) {
        w.str(id);
        w.str(std::string_view(reinterpret_cast<const char*>(hash.data()), hash.size()));
    }
}

bool readList(bin::Reader& r, std::vector<std::pair<std::string, Hash32>>& out) {
    std::uint64_t n = r.u64();
    if (n > kMaxEntries || n > r.remaining()) {
        return false;
    }
    for (std::uint64_t i = 0; i < n && !r.failed(); ++i) {
        std::string id = r.str();
        std::string h = r.str();
        if (h.size() != 32) {
            return false;
        }
        Hash32 hash{};
        std::copy(h.begin(), h.end(), hash.begin());
        out.emplace_back(std::move(id), hash);
    }
    return !r.failed();
}

} // namespace

std::vector<std::uint8_t> encodeStamp(const Stamp& stamp) {
    bin::Writer w;
    w.u64(kVersion);
    writeList(w, stamp.inputs);
    writeList(w, stamp.outputs);
    std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
    out.insert(out.end(), w.out.begin(), w.out.end());
    return out;
}

std::optional<Stamp> decodeStamp(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kMagic.size() || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return std::nullopt;
    }
    bin::Reader r(bytes.subspan(kMagic.size()));
    Stamp s;
    if (r.u64() != kVersion || !readList(r, s.inputs) || !readList(r, s.outputs) || !r.atEnd()) {
        return std::nullopt;
    }
    return s;
}

} // namespace qiln
