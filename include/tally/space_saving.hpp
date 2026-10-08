#pragma once

#include "tally/detail/codec.hpp"
#include "tally/hash.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tally {

struct ItemCount {
    std::string key;
    std::uint64_t count = 0;
    std::uint64_t error = 0;

    friend bool operator==(const ItemCount& left, const ItemCount& right) noexcept {
        return left.key == right.key && left.count == right.count && left.error == right.error;
    }
};

// Space-Saving heavy-hitters summary (Metwally, Agrawal, El Abbadi, 2005).
//
// Keeps k counters. A monitored key's true count is in [count - error, count].
// A key that fell out of the summary has true count <= the smallest counter.
// When several counters share that minimum, the lexicographically smallest key
// is the one replaced, so the summary does not depend on hash-table order.
class SpaceSaving {
public:
    explicit SpaceSaving(std::size_t k);

    void add(std::string_view item, std::uint64_t count = 1);
    [[nodiscard]] bool monitored(std::string_view item) const;
    [[nodiscard]] std::uint64_t upper_bound(std::string_view item) const;
    [[nodiscard]] std::uint64_t lower_bound(std::string_view item) const;
    [[nodiscard]] std::vector<ItemCount> top() const;

    [[nodiscard]] std::size_t k() const noexcept { return k_; }
    [[nodiscard]] std::size_t size() const noexcept { return counters_.size(); }
    [[nodiscard]] std::size_t size_bytes() const noexcept;

    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static SpaceSaving deserialize(std::string_view bytes);

    friend bool operator==(const SpaceSaving& left, const SpaceSaving& right) noexcept {
        return left.k_ == right.k_ && left.counters_ == right.counters_;
    }

private:
    struct Counter {
        std::uint64_t count = 0;
        std::uint64_t error = 0;

        friend bool operator==(const Counter& left, const Counter& right) noexcept {
            return left.count == right.count && left.error == right.error;
        }
    };

    using Table = std::unordered_map<std::string, Counter, StringHash, StringEqual>;

    [[nodiscard]] Table::const_iterator minimum() const;

    std::size_t k_ = 0;
    Table counters_;
};

inline SpaceSaving::SpaceSaving(std::size_t k) : k_(k) {
    detail::require(k > 0, "k must be positive");
    constexpr std::size_t kMaxCounters = 1'000'000;
    detail::require(k <= kMaxCounters, "space-saving summary is larger than the supported size");
    counters_.reserve(k);
}

inline void SpaceSaving::add(std::string_view item, std::uint64_t count) {
    if (count == 0) {
        return;
    }
    if (const auto found = counters_.find(item); found != counters_.end()) {
        found->second.count = detail::sat_add(found->second.count, count);
        return;
    }
    if (counters_.size() < k_) {
        counters_.emplace(std::string(item), Counter{count, 0});
        return;
    }

    const auto victim = minimum();
    const std::uint64_t floor = victim->second.count;
    counters_.erase(victim);
    counters_.emplace(std::string(item), Counter{detail::sat_add(floor, count), floor});
}

inline bool SpaceSaving::monitored(std::string_view item) const {
    return counters_.find(item) != counters_.end();
}

inline std::uint64_t SpaceSaving::upper_bound(std::string_view item) const {
    if (const auto found = counters_.find(item); found != counters_.end()) {
        return found->second.count;
    }
    if (counters_.empty()) {
        return 0;
    }
    return minimum()->second.count;
}

inline std::uint64_t SpaceSaving::lower_bound(std::string_view item) const {
    const auto found = counters_.find(item);
    if (found == counters_.end()) {
        return 0;
    }
    return found->second.count - found->second.error;
}

inline std::vector<ItemCount> SpaceSaving::top() const {
    std::vector<ItemCount> rows;
    rows.reserve(counters_.size());
    for (const auto& [key, counter] : counters_) {
        rows.push_back(ItemCount{key, counter.count, counter.error});
    }
    std::sort(rows.begin(), rows.end(), [](const ItemCount& left, const ItemCount& right) {
        if (left.count != right.count) {
            return left.count > right.count;
        }
        return left.key < right.key;
    });
    return rows;
}

inline std::size_t SpaceSaving::size_bytes() const noexcept {
    std::size_t bytes = counters_.size() * sizeof(Counter);
    for (const auto& entry : counters_) {
        bytes += entry.first.size();
    }
    return bytes;
}

inline std::string SpaceSaving::serialize() const {
    std::vector<ItemCount> rows = top();
    std::sort(rows.begin(), rows.end(), [](const ItemCount& left, const ItemCount& right) {
        return left.key < right.key;
    });

    detail::ByteWriter out;
    out.raw("SSV1", 4);
    out.u32(detail::kFormatVersion);
    out.u64(static_cast<std::uint64_t>(k_));
    out.u64(static_cast<std::uint64_t>(rows.size()));
    for (const ItemCount& row : rows) {
        out.str(row.key);
        out.u64(row.count);
        out.u64(row.error);
    }
    return out.take();
}

inline SpaceSaving SpaceSaving::deserialize(std::string_view bytes) {
    detail::ByteReader in(bytes);
    in.magic('S', 'S', 'V', '1');
    const auto k = static_cast<std::size_t>(in.u64());
    const std::uint64_t count = in.u64();
    SpaceSaving fresh(k);
    detail::require(count <= k, "space-saving snapshot has more counters than it allows");
    for (std::uint64_t i = 0; i < count; ++i) {
        std::string key = in.str();
        const std::uint64_t hits = in.u64();
        const std::uint64_t error = in.u64();
        detail::require(error <= hits, "space-saving snapshot has error larger than count");
        detail::require(!fresh.monitored(key), "space-saving snapshot repeats a key");
        fresh.counters_.emplace(std::move(key), Counter{hits, error});
    }
    in.finish();
    return fresh;
}

inline SpaceSaving::Table::const_iterator SpaceSaving::minimum() const {
    return std::min_element(counters_.begin(), counters_.end(), [](const auto& left, const auto& right) {
        if (left.second.count != right.second.count) {
            return left.second.count < right.second.count;
        }
        return left.first < right.first;
    });
}

}  // namespace tally
