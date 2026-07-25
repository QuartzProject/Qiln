#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace qiln {

using Hash32 = std::array<std::uint8_t, 32>;
using PublicKey = std::array<std::uint8_t, 32>;
using Seed = std::array<std::uint8_t, 32>;
using Signature = std::array<std::uint8_t, 64>;

std::string toHex(std::span<const std::uint8_t> bytes);

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> fromHex(std::string_view text);

extern template std::optional<std::array<std::uint8_t, 32>> fromHex<32>(std::string_view);
extern template std::optional<std::array<std::uint8_t, 64>> fromHex<64>(std::string_view);

/// BLAKE2b-256: what `.qzmeta` and `qiln.lock` pin.
Hash32 blake2b256(std::span<const std::uint8_t> bytes);
/// The digests Maven repositories publish beside each file, in lower case hex.
std::string sha256Hex(std::span<const std::uint8_t> bytes);
std::string sha512Hex(std::span<const std::uint8_t> bytes);

/// Incremental BLAKE2b-256, for hashing many inputs without joining them.
class Hasher {
public:
    Hasher();
    ~Hasher();
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;
    /// Adds the length first, so ("ab", "c") and ("a", "bc") differ.
    Hasher& add(std::string_view text);
    Hash32 finish();

private:
    struct State;
    std::unique_ptr<State> state_;
};

PublicKey publicKeyOf(const Seed& seed);
Signature sign(const Seed& seed, std::string_view message);
[[nodiscard]] bool verify(const PublicKey& key, const Signature& signature, std::string_view message);
/// 32 bytes from the operating system's random source.
Seed randomSeed();

} // namespace qiln
