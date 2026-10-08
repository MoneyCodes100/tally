#pragma once

#include "tally/detail/codec.hpp"
#include "tally/hash.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tally {

// Bloom filter (Bloom, 1970) with Kirsch-Mitzenmacher double hashing (2006).
//
// add() is permanent: a key that was inserted always comes back as present.
// might_contain() is allowed to say yes for a key that was never inserted.
// The expected false-positive rate is the error_rate passed to the constructor,
// once the filter holds about `capacity` distinct keys.
class BloomFilter {
public:
    BloomFilter(std::size_t capacity, double error_rate);

    void add(std::string_view item);
    [[nodiscard]] bool might_contain(std::string_view item) const;
    void merge(const BloomFilter& other);

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] double error_rate() const noexcept { return error_rate_; }
    [[nodiscard]] std::size_t bit_count() const noexcept { return bit_count_; }
    [[nodiscard]] int hash_count() const noexcept { return hash_count_; }
    [[nodiscard]] std::size_t size_bytes() const noexcept {
        return bits_.size() * sizeof(std::uint64_t);
    }
    // Share of bits that are set. Useful when you want the rate the filter is
    // actually producing, which grows as it fills.
    [[nodiscard]] double fill_ratio() const noexcept;

    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static BloomFilter deserialize(std::string_view bytes);

    friend bool operator==(const BloomFilter& left, const BloomFilter& right) noexcept {
        return left.capacity_ == right.capacity_ && left.error_rate_ == right.error_rate_ &&
               left.bit_count_ == right.bit_count_ && left.hash_count_ == right.hash_count_ &&
               left.bits_ == right.bits_;
    }

private:
    template <typename Fn>
    void for_each_index(std::string_view item, Fn&& visit) const;

    void clear_unused_bits() noexcept;

    std::size_t capacity_ = 0;
    double error_rate_ = 0;
    std::size_t bit_count_ = 0;
    int hash_count_ = 0;
    std::vector<std::uint64_t> bits_;
};

inline BloomFilter::BloomFilter(std::size_t capacity, double error_rate)
    : capacity_(capacity), error_rate_(error_rate) {
    detail::require(capacity > 0, "capacity must be positive");
    detail::require(std::isfinite(error_rate) && error_rate > 0.0 && error_rate < 1.0,
                    "error_rate must be between 0 and 1");

    constexpr double kLn2 = 0.693147180559945309417;
    constexpr std::size_t kMaxBits = std::size_t{1} << 28;
    const double bits = -static_cast<double>(capacity) * std::log(error_rate) / (kLn2 * kLn2);
    detail::require(bits > 0.0 && bits <= static_cast<double>(kMaxBits),
                    "bloom filter parameters need more than the supported number of bits");

    bit_count_ = static_cast<std::size_t>(std::ceil(bits));
    if (bit_count_ < 1) {
        bit_count_ = 1;
    }

    const double hashes = (static_cast<double>(bit_count_) / static_cast<double>(capacity)) * kLn2;
    hash_count_ = static_cast<int>(std::llround(hashes));
    if (hash_count_ < 1) {
        hash_count_ = 1;
    }
    detail::require(hash_count_ <= 64,
                    "error_rate is so small that the filter would need more than 64 hashes");

    bits_.assign((bit_count_ + 63) / 64, 0);
}

inline void BloomFilter::add(std::string_view item) {
    for_each_index(item, [this](std::size_t index) {
        bits_[index >> 6] |= std::uint64_t{1} << (index & 63U);
    });
}

inline bool BloomFilter::might_contain(std::string_view item) const {
    bool present = true;
    for_each_index(item, [this, &present](std::size_t index) {
        present = present && ((bits_[index >> 6] >> (index & 63U)) & 1U);
    });
    return present;
}

inline void BloomFilter::merge(const BloomFilter& other) {
    if (this == &other) {
        return;
    }
    detail::require(bit_count_ == other.bit_count_ && hash_count_ == other.hash_count_,
                    "cannot merge Bloom filters with different parameters");
    for (std::size_t i = 0; i < bits_.size(); ++i) {
        bits_[i] |= other.bits_[i];
    }
}

inline double BloomFilter::fill_ratio() const noexcept {
    std::size_t set = 0;
    for (std::uint64_t word : bits_) {
        set += static_cast<std::size_t>(std::popcount(word));
    }
    return bit_count_ == 0 ? 0.0 : static_cast<double>(set) / static_cast<double>(bit_count_);
}

inline std::string BloomFilter::serialize() const {
    detail::ByteWriter out;
    out.raw("BLM1", 4);
    out.u32(detail::kFormatVersion);
    out.u64(static_cast<std::uint64_t>(capacity_));
    out.f64(error_rate_);
    out.u64(static_cast<std::uint64_t>(bit_count_));
    out.u32(static_cast<std::uint32_t>(hash_count_));
    out.u64(static_cast<std::uint64_t>(bits_.size()));
    out.raw(bits_.data(), bits_.size() * sizeof(std::uint64_t));
    return out.take();
}

inline BloomFilter BloomFilter::deserialize(std::string_view bytes) {
    detail::ByteReader in(bytes);
    in.magic('B', 'L', 'M', '1');
    const auto capacity = static_cast<std::size_t>(in.u64());
    const double error_rate = in.f64();
    const auto bit_count = static_cast<std::size_t>(in.u64());
    const int hash_count = static_cast<int>(in.u32());
    const std::uint64_t words = in.u64();
    std::vector<std::uint64_t> bits;
    in.vec(bits, words);
    in.finish();

    BloomFilter fresh(capacity, error_rate);
    detail::require(fresh.bit_count_ == bit_count && fresh.hash_count_ == hash_count &&
                        fresh.bits_.size() == bits.size(),
                    "bloom filter snapshot does not match its parameters");
    fresh.bits_ = std::move(bits);
    fresh.clear_unused_bits();
    return fresh;
}

template <typename Fn>
inline void BloomFilter::for_each_index(std::string_view item, Fn&& visit) const {
    const Hash128 hash = murmur3_x64_128(item);
    const std::uint64_t h1 = hash.h1;
    const std::uint64_t h2 = hash.h2 | 1ULL;  // odd step avoids a short cycle
    for (int i = 0; i < hash_count_; ++i) {
        const std::uint64_t mixed = h1 + static_cast<std::uint64_t>(i) * h2;
        visit(detail::fastrange(mixed, bit_count_));
    }
}

inline void BloomFilter::clear_unused_bits() noexcept {
    const std::size_t used = bit_count_ & 63U;
    if (used == 0 || bits_.empty()) {
        return;
    }
    const std::uint64_t mask = (std::uint64_t{1} << used) - 1ULL;
    bits_.back() &= mask;
}

}  // namespace tally
