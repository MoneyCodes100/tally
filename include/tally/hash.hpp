#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace tally {

// 128-bit MurmurHash3, x64 variant.
//
// Austin Appleby released MurmurHash3 into the public domain. This is the
// little-endian x64_128 function: the same one the mmh3 Python package
// exposes as hash64(..., x64arch=True). The sketches share it so a key lands
// in the same place in C++ and in Python.
struct Hash128 {
    std::uint64_t h1 = 0;
    std::uint64_t h2 = 0;

    friend bool operator==(const Hash128& a, const Hash128& b) noexcept {
        return a.h1 == b.h1 && a.h2 == b.h2;
    }
};

inline std::uint64_t rotl64(std::uint64_t value, int shift) noexcept {
    return (value << shift) | (value >> (64 - shift));
}

inline std::uint64_t load_le64(const std::uint8_t* data) noexcept {
    std::uint64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

inline Hash128 murmur3_x64_128(std::string_view key, std::uint32_t seed = 0) noexcept {
    const auto* data = reinterpret_cast<const std::uint8_t*>(key.data());
    const std::size_t len = key.size();
    const std::size_t nblocks = len / 16;

    std::uint64_t h1 = seed;
    std::uint64_t h2 = seed;
    constexpr std::uint64_t c1 = 0x87c37b91114253d5ULL;
    constexpr std::uint64_t c2 = 0x4cf5ad432745937fULL;

    for (std::size_t i = 0; i < nblocks; ++i) {
        const std::uint8_t* block = data + i * 16;
        std::uint64_t k1 = load_le64(block);
        std::uint64_t k2 = load_le64(block + 8);

        k1 *= c1;
        k1 = rotl64(k1, 31);
        k1 *= c2;
        h1 ^= k1;
        h1 = rotl64(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729;

        k2 *= c2;
        k2 = rotl64(k2, 33);
        k2 *= c1;
        h2 ^= k2;
        h2 = rotl64(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5;
    }

    const std::uint8_t* tail = data + nblocks * 16;
    std::uint64_t k1 = 0;
    std::uint64_t k2 = 0;

    switch (len & 15U) {
        case 15:
            k2 ^= static_cast<std::uint64_t>(tail[14]) << 48;
            [[fallthrough]];
        case 14:
            k2 ^= static_cast<std::uint64_t>(tail[13]) << 40;
            [[fallthrough]];
        case 13:
            k2 ^= static_cast<std::uint64_t>(tail[12]) << 32;
            [[fallthrough]];
        case 12:
            k2 ^= static_cast<std::uint64_t>(tail[11]) << 24;
            [[fallthrough]];
        case 11:
            k2 ^= static_cast<std::uint64_t>(tail[10]) << 16;
            [[fallthrough]];
        case 10:
            k2 ^= static_cast<std::uint64_t>(tail[9]) << 8;
            [[fallthrough]];
        case 9:
            k2 ^= static_cast<std::uint64_t>(tail[8]);
            k2 *= c2;
            k2 = rotl64(k2, 33);
            k2 *= c1;
            h2 ^= k2;
            [[fallthrough]];
        case 8:
            k1 ^= static_cast<std::uint64_t>(tail[7]) << 56;
            [[fallthrough]];
        case 7:
            k1 ^= static_cast<std::uint64_t>(tail[6]) << 48;
            [[fallthrough]];
        case 6:
            k1 ^= static_cast<std::uint64_t>(tail[5]) << 40;
            [[fallthrough]];
        case 5:
            k1 ^= static_cast<std::uint64_t>(tail[4]) << 32;
            [[fallthrough]];
        case 4:
            k1 ^= static_cast<std::uint64_t>(tail[3]) << 24;
            [[fallthrough]];
        case 3:
            k1 ^= static_cast<std::uint64_t>(tail[2]) << 16;
            [[fallthrough]];
        case 2:
            k1 ^= static_cast<std::uint64_t>(tail[1]) << 8;
            [[fallthrough]];
        case 1:
            k1 ^= static_cast<std::uint64_t>(tail[0]);
            k1 *= c1;
            k1 = rotl64(k1, 31);
            k1 *= c2;
            h1 ^= k1;
            break;
        default:
            break;
    }

    h1 ^= static_cast<std::uint64_t>(len);
    h2 ^= static_cast<std::uint64_t>(len);
    h1 += h2;
    h2 += h1;

    auto fmix = [](std::uint64_t k) noexcept {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return k;
    };
    h1 = fmix(h1);
    h2 = fmix(h2);
    h1 += h2;
    h2 += h1;
    return Hash128{h1, h2};
}

// Transparent hash/equality so sketches can look up a string_view without
// allocating a temporary std::string.
struct StringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view text) const noexcept {
        return static_cast<std::size_t>(murmur3_x64_128(text).h1);
    }
};

struct StringEqual {
    using is_transparent = void;

    bool operator()(std::string_view left, std::string_view right) const noexcept {
        return left == right;
    }
};

}  // namespace tally
