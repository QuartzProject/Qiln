#include "support/Hash.hpp"

#include "std/Sha.hpp"

#include <monocypher/monocypher-ed25519.h>
#include <monocypher/monocypher.h>
#include <random>

namespace qiln {

std::string toHex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
    return out;
}

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> fromHex(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    if (text.size() != N * 2) {
        return std::nullopt;
    }
    auto digit = [](char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    };
    std::array<std::uint8_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        int hi = digit(text[2 * i]);
        int lo = digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return out;
}

template std::optional<std::array<std::uint8_t, 32>> fromHex<32>(std::string_view);
template std::optional<std::array<std::uint8_t, 64>> fromHex<64>(std::string_view);

Hash32 blake2b256(std::span<const std::uint8_t> bytes) {
    Hash32 h{};
    crypto_blake2b(h.data(), h.size(), bytes.data(), bytes.size());
    return h;
}

std::string sha256Hex(std::span<const std::uint8_t> bytes) {
    quartz::stdlib::Sha256 sha;
    sha.update(bytes);
    return toHex(sha.final());
}

std::string sha512Hex(std::span<const std::uint8_t> bytes) {
    std::array<std::uint8_t, 64> h{};
    crypto_sha512(h.data(), bytes.data(), bytes.size());
    return toHex(h);
}

struct Hasher::State {
    crypto_blake2b_ctx ctx;
};

Hasher::Hasher() : state_(std::make_unique<State>()) { crypto_blake2b_init(&state_->ctx, 32); }

Hasher::~Hasher() = default;

Hasher& Hasher::add(std::string_view text) {
    std::array<std::uint8_t, 8> size{};
    for (std::size_t i = 0; i < size.size(); ++i) {
        size[i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(text.size()) >> (8 * i));
    }
    crypto_blake2b_update(&state_->ctx, size.data(), size.size());
    crypto_blake2b_update(&state_->ctx, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return *this;
}

Hash32 Hasher::finish() {
    Hash32 h{};
    crypto_blake2b_final(&state_->ctx, h.data());
    return h;
}

PublicKey publicKeyOf(const Seed& seed) {
    std::array<std::uint8_t, 64> secret{};
    PublicKey key{};
    Seed scratch = seed;
    crypto_ed25519_key_pair(secret.data(), key.data(), scratch.data());
    crypto_wipe(secret.data(), secret.size());
    return key;
}

Signature sign(const Seed& seed, std::string_view message) {
    std::array<std::uint8_t, 64> secret{};
    PublicKey key{};
    Seed scratch = seed;
    crypto_ed25519_key_pair(secret.data(), key.data(), scratch.data());
    Signature out{};
    crypto_ed25519_sign(out.data(), secret.data(), reinterpret_cast<const std::uint8_t*>(message.data()),
                        message.size());
    crypto_wipe(secret.data(), secret.size());
    return out;
}

bool verify(const PublicKey& key, const Signature& signature, std::string_view message) {
    return crypto_ed25519_check(signature.data(), key.data(), reinterpret_cast<const std::uint8_t*>(message.data()),
                                message.size()) == 0;
}

Seed randomSeed() {
    std::random_device rd;
    Seed seed{};
    for (std::size_t i = 0; i < seed.size(); i += 4) {
        std::uint32_t r = rd();
        for (std::size_t k = 0; k < 4; ++k) {
            seed[i + k] = static_cast<std::uint8_t>(r >> (8 * k));
        }
    }
    return seed;
}

} // namespace qiln
