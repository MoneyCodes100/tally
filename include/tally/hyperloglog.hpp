#pragma once

#include "tally/detail/codec.hpp"
#include "tally/hash.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tally {

// HyperLogLog cardinality sketch (Flajolet, Fusy, Gandouet, Meunier, 2007).
//
// Each key sets one register to the length of the leading-zero run in its
// hash. The harmonic mean of those registers estimates how many distinct keys
// went past. Memory is 2^precision bytes and does not grow with the stream.
// The hash is 64-bit, so the 32-bit large-range correction is omitted.
class HyperLogLog {
public:
    explicit HyperLogLog(int precision = 14);

    void add(std::string_view item);
    void merge(const HyperLogLog& other);
    [[nodiscard]] double estimate() const;

    [[nodiscard]] int precision() const noexcept { return precision_; }
    [[nodiscard]] std::size_t size_bytes() const noexcept { return registers_.size(); }

    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static HyperLogLog deserialize(std::string_view bytes);

    friend bool operator==(const HyperLogLog& left, const HyperLogLog& right) noexcept {
        return left.precision_ == right.precision_ && left.registers_ == right.registers_;
    }

private:
    static double alpha(std::size_t registers);

    int precision_ = 14;
    double alpha_ = 0;
    std::vector<std::uint8_t> registers_;
};

inline HyperLogLog::HyperLogLog(int precision) : precision_(precision) {
    detail::require(precision >= 4 && precision <= 16, "precision must be between 4 and 16");
    registers_.assign(std::size_t{1} << precision_, 0);
    alpha_ = alpha(registers_.size());
}

inline void HyperLogLog::add(std::string_view item) {
    const std::uint64_t hash = murmur3_x64_128(item).h1;
    const std::size_t index = static_cast<std::size_t>(hash >> (64 - precision_));
    const std::uint64_t rest = hash << precision_;
    int rank = 0;
    if (rest == 0) {
        rank = (64 - precision_) + 1;
    } else {
        rank = std::countl_zero(rest) + 1;
    }
    auto& slot = registers_[index];
    if (rank > static_cast<int>(slot)) {
        slot = static_cast<std::uint8_t>(rank);
    }
}

inline void HyperLogLog::merge(const HyperLogLog& other) {
    if (this == &other) {
        return;
    }
    detail::require(precision_ == other.precision_,
                    "cannot merge HyperLogLog sketches with different precision");
    for (std::size_t i = 0; i < registers_.size(); ++i) {
        if (other.registers_[i] > registers_[i]) {
            registers_[i] = other.registers_[i];
        }
    }
}

inline double HyperLogLog::estimate() const {
    const double m = static_cast<double>(registers_.size());
    double sum = 0;
    int zeros = 0;
    for (std::uint8_t rank : registers_) {
        sum += std::ldexp(1.0, -static_cast<int>(rank));
        if (rank == 0) {
            ++zeros;
        }
    }
    double estimate = alpha_ * m * m / sum;
    // Linear counting is much less biased while the sketch is still sparse.
    if (estimate <= 2.5 * m && zeros > 0) {
        estimate = m * std::log(m / static_cast<double>(zeros));
    }
    return estimate;
}

inline std::string HyperLogLog::serialize() const {
    detail::ByteWriter out;
    out.raw("HLL1", 4);
    out.u32(detail::kFormatVersion);
    out.u32(static_cast<std::uint32_t>(precision_));
    out.raw(registers_.data(), registers_.size());
    return out.take();
}

inline HyperLogLog HyperLogLog::deserialize(std::string_view bytes) {
    detail::ByteReader in(bytes);
    in.magic('H', 'L', 'L', '1');
    const int precision = static_cast<int>(in.u32());
    HyperLogLog fresh(precision);
    in.raw(fresh.registers_.data(), fresh.registers_.size());
    in.finish();
    return fresh;
}

inline double HyperLogLog::alpha(std::size_t registers) {
    if (registers == 16) {
        return 0.673;
    }
    if (registers == 32) {
        return 0.697;
    }
    if (registers == 64) {
        return 0.709;
    }
    return 0.7213 / (1.0 + 1.079 / static_cast<double>(registers));
}

}  // namespace tally
