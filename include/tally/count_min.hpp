#pragma once

#include "tally/detail/codec.hpp"
#include "tally/hash.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace tally {

// Count-Min Sketch (Cormode and Muthukrishnan, 2005).
//
// update is a handful of counter increments. estimate(key) is the minimum
// counter across the rows that key hashes to, so it is always at least the
// true count. With width ~= e / epsilon and depth ~= ln(1 / delta), the
// overestimate is at most epsilon * (total count) with probability 1 - delta.
// Two sketches of the same shape merge by adding their counters.
class CountMinSketch {
public:
    CountMinSketch(std::size_t width, std::size_t depth);
    [[nodiscard]] static CountMinSketch from_error(double epsilon, double delta);

    void add(std::string_view item, std::uint64_t count = 1);
    [[nodiscard]] std::uint64_t estimate(std::string_view item) const;
    void merge(const CountMinSketch& other);

    [[nodiscard]] std::size_t width() const noexcept { return width_; }
    [[nodiscard]] std::size_t depth() const noexcept { return depth_; }
    [[nodiscard]] std::size_t size_bytes() const noexcept {
        return cells_.size() * sizeof(std::uint64_t);
    }

    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static CountMinSketch deserialize(std::string_view bytes);

    friend bool operator==(const CountMinSketch& left, const CountMinSketch& right) noexcept {
        return left.width_ == right.width_ && left.depth_ == right.depth_ && left.cells_ == right.cells_;
    }

private:
    [[nodiscard]] std::size_t index(std::string_view item, std::size_t row) const;

    std::size_t width_ = 0;
    std::size_t depth_ = 0;
    std::vector<std::uint64_t> cells_;
};

inline CountMinSketch::CountMinSketch(std::size_t width, std::size_t depth)
    : width_(width), depth_(depth) {
    detail::require(width >= 2, "width must be at least 2");
    detail::require(depth >= 1 && depth <= 64, "depth must be between 1 and 64");
    constexpr std::size_t kMaxCells = 20'000'000;
    detail::require(width <= kMaxCells / depth, "count-min sketch is larger than the supported size");
    cells_.assign(width * depth, 0);
}

inline CountMinSketch CountMinSketch::from_error(double epsilon, double delta) {
    detail::require(std::isfinite(epsilon) && epsilon > 0.0 && epsilon < 1.0,
                    "epsilon must be between 0 and 1");
    detail::require(std::isfinite(delta) && delta > 0.0 && delta < 1.0, "delta must be between 0 and 1");
    const auto width = static_cast<std::size_t>(std::ceil(std::exp(1.0) / epsilon));
    const auto depth = static_cast<std::size_t>(std::ceil(std::log(1.0 / delta)));
    return CountMinSketch(width, depth < 1 ? 1 : depth);
}

inline void CountMinSketch::add(std::string_view item, std::uint64_t count) {
    if (count == 0) {
        return;
    }
    for (std::size_t row = 0; row < depth_; ++row) {
        std::uint64_t& cell = cells_[row * width_ + index(item, row)];
        cell = detail::sat_add(cell, count);
    }
}

inline std::uint64_t CountMinSketch::estimate(std::string_view item) const {
    std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t row = 0; row < depth_; ++row) {
        const std::uint64_t cell = cells_[row * width_ + index(item, row)];
        if (cell < best) {
            best = cell;
        }
    }
    return best;
}

inline void CountMinSketch::merge(const CountMinSketch& other) {
    if (this == &other) {
        return;
    }
    detail::require(width_ == other.width_ && depth_ == other.depth_,
                    "cannot merge count-min sketches with different parameters");
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        cells_[i] = detail::sat_add(cells_[i], other.cells_[i]);
    }
}

inline std::string CountMinSketch::serialize() const {
    detail::ByteWriter out;
    out.raw("CMS1", 4);
    out.u32(detail::kFormatVersion);
    out.u64(static_cast<std::uint64_t>(width_));
    out.u64(static_cast<std::uint64_t>(depth_));
    out.raw(cells_.data(), cells_.size() * sizeof(std::uint64_t));
    return out.take();
}

inline CountMinSketch CountMinSketch::deserialize(std::string_view bytes) {
    detail::ByteReader in(bytes);
    in.magic('C', 'M', 'S', '1');
    const auto width = static_cast<std::size_t>(in.u64());
    const auto depth = static_cast<std::size_t>(in.u64());
    CountMinSketch fresh(width, depth);
    in.raw(fresh.cells_.data(), fresh.cells_.size() * sizeof(std::uint64_t));
    in.finish();
    return fresh;
}

inline std::size_t CountMinSketch::index(std::string_view item, std::size_t row) const {
    const std::uint32_t seed = 0x85EBCA6Bu ^ (static_cast<std::uint32_t>(row) * 0x9E3779B9u);
    const Hash128 hash = murmur3_x64_128(item, seed);
    return detail::fastrange(hash.h1 ^ hash.h2, width_);
}

}  // namespace tally
