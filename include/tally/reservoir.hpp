#pragma once

#include "tally/detail/codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tally {

// Reservoir sample (Vitter, Algorithm R, 1985).
//
// Keeps a uniform sample of a fixed size, however long the stream is. Quantiles
// are read from that sample. The first `capacity` values are kept in order;
// randomness starts only once the reservoir is full. The generator is SplitMix64
// so a snapshot restores the same sequence on any little-endian machine.
class Reservoir {
public:
    explicit Reservoir(std::size_t capacity, std::uint64_t seed = 0x5EED1234C0FFEEULL);

    void add(double value);
    [[nodiscard]] double quantile(double q) const;
    [[nodiscard]] std::vector<double> sample() const { return sample_; }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t size() const noexcept { return sample_.size(); }
    [[nodiscard]] std::uint64_t seen() const noexcept { return seen_; }
    [[nodiscard]] std::size_t size_bytes() const noexcept { return sample_.size() * sizeof(double); }

    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static Reservoir deserialize(std::string_view bytes);

    friend bool operator==(const Reservoir& left, const Reservoir& right) noexcept {
        return left.capacity_ == right.capacity_ && left.seen_ == right.seen_ &&
               left.rng_.state() == right.rng_.state() && left.sample_ == right.sample_;
    }

private:
    class SplitMix64 {
    public:
        explicit SplitMix64(std::uint64_t state = 0) : state_(state) {}

        std::uint64_t next() noexcept {
            std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        }

        // Unbiased integer in [0, inclusive_max].
        std::uint64_t uniform(std::uint64_t inclusive_max) noexcept {
            if (inclusive_max == std::numeric_limits<std::uint64_t>::max()) {
                return next();
            }
            const std::uint64_t range = inclusive_max + 1;
            const std::uint64_t limit = std::numeric_limits<std::uint64_t>::max() / range * range;
            std::uint64_t draw = next();
            while (draw >= limit) {
                draw = next();
            }
            return draw % range;
        }

        std::uint64_t state() const noexcept { return state_; }

    private:
        std::uint64_t state_ = 0;
    };

    std::size_t capacity_ = 0;
    std::uint64_t seen_ = 0;
    SplitMix64 rng_;
    std::vector<double> sample_;
};

inline Reservoir::Reservoir(std::size_t capacity, std::uint64_t seed)
    : capacity_(capacity), rng_(seed) {
    detail::require(capacity > 0, "capacity must be positive");
    constexpr std::size_t kMaxCapacity = 5'000'000;
    detail::require(capacity <= kMaxCapacity, "reservoir is larger than the supported size");
    sample_.reserve(capacity);
}

inline void Reservoir::add(double value) {
    detail::require(std::isfinite(value), "reservoir values must be finite");
    if (sample_.size() < capacity_) {
        sample_.push_back(value);
    } else {
        const std::uint64_t slot = rng_.uniform(seen_);
        if (slot < capacity_) {
            sample_[static_cast<std::size_t>(slot)] = value;
        }
    }
    if (seen_ != std::numeric_limits<std::uint64_t>::max()) {
        ++seen_;
    }
}

inline double Reservoir::quantile(double q) const {
    detail::require(!sample_.empty(), "reservoir is empty");
    detail::require(std::isfinite(q) && q >= 0.0 && q <= 1.0, "quantile must be between 0 and 1");
    std::vector<double> ordered = sample_;
    std::sort(ordered.begin(), ordered.end());
    if (ordered.size() == 1) {
        return ordered[0];
    }
    const double position = q * static_cast<double>(ordered.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    return std::lerp(ordered[lower], ordered[upper], position - static_cast<double>(lower));
}

inline std::string Reservoir::serialize() const {
    detail::ByteWriter out;
    out.raw("RSV1", 4);
    out.u32(detail::kFormatVersion);
    out.u64(static_cast<std::uint64_t>(capacity_));
    out.u64(seen_);
    out.u64(rng_.state());
    out.u64(static_cast<std::uint64_t>(sample_.size()));
    out.raw(sample_.data(), sample_.size() * sizeof(double));
    return out.take();
}

inline Reservoir Reservoir::deserialize(std::string_view bytes) {
    detail::ByteReader in(bytes);
    in.magic('R', 'S', 'V', '1');
    const auto capacity = static_cast<std::size_t>(in.u64());
    const std::uint64_t seen = in.u64();
    const std::uint64_t state = in.u64();
    const std::uint64_t count = in.u64();
    Reservoir fresh(capacity, state);
    detail::require(count <= capacity, "reservoir snapshot is larger than its capacity");
    detail::require(seen >= count, "reservoir snapshot has seen fewer values than it stored");
    if (seen >= capacity) {
        detail::require(count == capacity, "a full reservoir snapshot must store every slot");
    } else {
        detail::require(count == seen, "a filling reservoir snapshot must store every value seen");
    }
    fresh.sample_.resize(static_cast<std::size_t>(count));
    in.raw(fresh.sample_.data(), fresh.sample_.size() * sizeof(double));
    in.finish();
    fresh.seen_ = seen;
    for (double value : fresh.sample_) {
        detail::require(std::isfinite(value), "reservoir snapshot contains a non-finite value");
    }
    return fresh;
}

}  // namespace tally
