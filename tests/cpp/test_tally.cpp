#include "tally/tally.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool condition, const char* text, const char* file, int line) {
    if (condition) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::cerr << file << ":" << line << " failed: " << text << "\n";
}

template <typename A, typename B>
void check_eq(const A& left, const B& right, const char* text, const char* file, int line) {
    if (left == right) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::cerr << file << ":" << line << " failed: " << text << "\n  left=" << left << "\n  right=" << right
              << "\n";
}

void check_near(double left, double right, double tolerance, const char* text, const char* file, int line) {
    if (std::abs(left - right) <= tolerance) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::cerr << file << ":" << line << " failed: " << text << "\n  left=" << left << "\n  right=" << right
              << "\n";
}

void check_rel(double estimate, double exact, double tolerance, const char* file, int line) {
    const double error = std::abs(estimate - exact) / exact;
    if (error <= tolerance) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::cerr << file << ":" << line << " relative error " << error << " estimate=" << estimate
              << " exact=" << exact << "\n";
}

#define CHECK(cond) ::check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::check_eq((a), (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) ::check_near((a), (b), (tol), #a " ~ " #b, __FILE__, __LINE__)

std::string key_at(int index) { return "key-" + std::to_string(index); }

void expect_hash(std::string_view text, std::uint32_t seed, std::uint64_t h1, std::uint64_t h2) {
    const tally::Hash128 hash = tally::murmur3_x64_128(text, seed);
    if (hash.h1 == h1 && hash.h2 == h2) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::cerr << "hash mismatch for [" << text << "] seed " << seed << "\n  got " << std::hex << hash.h1 << " "
              << hash.h2 << "\n  want " << h1 << " " << h2 << std::dec << "\n";
}

void test_hash() {
    expect_hash("", 0, 0, 0);
    expect_hash("a", 0, 0x85555565f6597889ULL, 0xe6b53a48510e895aULL);
    expect_hash("hello", 0, 0xcbd8a7b341bd9b02ULL, 0x5b1e906a48ae1d19ULL);
    expect_hash("Hello, world!", 0, 0xf1512dd1d2d665dfULL, 0x2c326650a8f3c564ULL);
    expect_hash("tally", 0, 0xee6f8d7f97827df8ULL, 0xb7578c16a0765296ULL);
    expect_hash("user-42", 0, 0x4d32de269f8ffaa2ULL, 0xec4e84934ab603beULL);
    expect_hash("abcdefghijklmnopqrstuvwxyz", 0, 0x749c9d7e516f4aa9ULL, 0xe9ad9c89b6a7d529ULL);
    expect_hash(std::string(17, 'a'), 0, 0x6f7214c7cef2d698ULL, 0xa4fdbc534edea5bbULL);
    expect_hash("The quick brown fox jumps over the lazy dog", 0, 0xe34bbc7bbc071b6cULL, 0x7a433ca9c49a9347ULL);
    expect_hash("hello", 42, 0xc4b8b3c960af6f08ULL, 0x2334b875b0efbc7aULL);

    for (std::uint64_t i = 0; i < 200; ++i) {
        const std::size_t slot = tally::detail::fastrange(i * 0x9E3779B97F4A7C15ULL, 97);
        CHECK(slot < 97);
    }
}

void test_bloom() {
    bool rejected = false;
    try {
        tally::BloomFilter broken(100, 0.0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);

    tally::BloomFilter filter(2000, 0.01);
    CHECK(filter.hash_count() >= 1);
    CHECK(filter.bit_count() > 2000);
    CHECK(filter.might_contain("missing") == false);

    constexpr int kInserted = 2000;
    for (int i = 0; i < kInserted; ++i) {
        filter.add(key_at(i));
    }
    for (int i = 0; i < kInserted; ++i) {
        CHECK(filter.might_contain(key_at(i)));
    }

    int false_positives = 0;
    constexpr int kAbsent = 2000;
    for (int i = 0; i < kAbsent; ++i) {
        if (filter.might_contain("absent-" + std::to_string(i))) {
            ++false_positives;
        }
    }
    CHECK(false_positives < kAbsent / 20);

    tally::BloomFilter left(500, 0.02);
    tally::BloomFilter right(500, 0.02);
    tally::BloomFilter both(500, 0.02);
    for (int i = 0; i < 200; ++i) {
        const std::string key = key_at(i);
        both.add(key);
        if (i % 2 == 0) {
            left.add(key);
        } else {
            right.add(key);
        }
    }
    left.merge(right);
    CHECK(left == both);

    const auto copy = tally::BloomFilter::deserialize(filter.serialize());
    CHECK(copy == filter);
    CHECK_NEAR(copy.fill_ratio(), filter.fill_ratio(), 1e-12);

    bool bad_merge = false;
    try {
        tally::BloomFilter other(100, 0.1);
        filter.merge(other);
    } catch (const std::invalid_argument&) {
        bad_merge = true;
    }
    CHECK(bad_merge);

    bool bad_bytes = false;
    try {
        (void)tally::BloomFilter::deserialize("nope");
    } catch (const std::invalid_argument&) {
        bad_bytes = true;
    }
    CHECK(bad_bytes);
}

void test_hyperloglog() {
    tally::HyperLogLog empty(14);
    CHECK_EQ(empty.estimate(), 0.0);
    CHECK_EQ(empty.size_bytes(), std::size_t{1} << 14);

    tally::HyperLogLog one(14);
    one.add("only");
    one.add("only");
    const double m = static_cast<double>(std::size_t{1} << 14);
    CHECK_NEAR(one.estimate(), m * std::log(m / (m - 1.0)), 1e-6);

    tally::HyperLogLog coarse(4);
    coarse.add("only");
    const double m4 = 16.0;
    CHECK_NEAR(coarse.estimate(), m4 * std::log(m4 / (m4 - 1.0)), 1e-9);

    constexpr int kCount = 50000;
    tally::HyperLogLog sketch(14);
    tally::HyperLogLog left(14);
    tally::HyperLogLog right(14);
    for (int i = 0; i < kCount; ++i) {
        const std::string key = key_at(i);
        sketch.add(key);
        sketch.add(key);
        if (i % 2 == 0) {
            left.add(key);
        } else {
            right.add(key);
        }
    }
    check_rel(sketch.estimate(), static_cast<double>(kCount), 0.03, __FILE__, __LINE__);
    left.merge(right);
    CHECK(left == sketch);

    const auto restored = tally::HyperLogLog::deserialize(sketch.serialize());
    CHECK(restored == sketch);
    CHECK_EQ(restored.estimate(), sketch.estimate());

    auto truncated = sketch.serialize();
    truncated.pop_back();
    bool rejected = false;
    try {
        (void)tally::HyperLogLog::deserialize(truncated);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);

    bool bad_precision = false;
    try {
        tally::HyperLogLog tiny(3);
        (void)tiny;
    } catch (const std::invalid_argument&) {
        bad_precision = true;
    }
    CHECK(bad_precision);
}

void test_count_min() {
    const auto sized = tally::CountMinSketch::from_error(0.01, 0.01);
    CHECK_EQ(sized.width(), static_cast<std::size_t>(272));
    CHECK_EQ(sized.depth(), static_cast<std::size_t>(5));

    tally::CountMinSketch only(64, 4);
    only.add("alpha", 7);
    CHECK_EQ(only.estimate("alpha"), static_cast<std::uint64_t>(7));
    CHECK_EQ(only.estimate("beta"), static_cast<std::uint64_t>(0));
    only.add("alpha", 0);
    CHECK_EQ(only.estimate("alpha"), static_cast<std::uint64_t>(7));

    tally::CountMinSketch left(128, 4);
    tally::CountMinSketch right(128, 4);
    left.add("alpha", 3);
    right.add("alpha", 4);
    left.merge(right);
    CHECK_EQ(left.estimate("alpha"), static_cast<std::uint64_t>(7));

    std::unordered_map<std::string, std::uint64_t> truth;
    tally::CountMinSketch mixed(512, 5);
    for (int i = 0; i < 300; ++i) {
        const std::string key = key_at(i % 40);
        const std::uint64_t weight = static_cast<std::uint64_t>((i % 5) + 1);
        mixed.add(key, weight);
        truth[key] += weight;
    }
    for (const auto& [key, count] : truth) {
        CHECK(mixed.estimate(key) >= count);
    }

    tally::CountMinSketch saturated(8, 2);
    saturated.add("a", std::numeric_limits<std::uint64_t>::max());
    saturated.add("a", 5);
    CHECK_EQ(saturated.estimate("a"), std::numeric_limits<std::uint64_t>::max());

    CHECK(tally::CountMinSketch::deserialize(mixed.serialize()) == mixed);

    bool rejected = false;
    try {
        (void)tally::CountMinSketch::from_error(1.0, 0.01);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

void test_reservoir() {
    tally::Reservoir sample(4, 99);
    sample.add(10);
    sample.add(20);
    sample.add(30);
    sample.add(40);
    CHECK_EQ(sample.size(), static_cast<std::size_t>(4));
    CHECK_EQ(sample.seen(), static_cast<std::uint64_t>(4));
    const auto values = sample.sample();
    CHECK_EQ(values.size(), static_cast<std::size_t>(4));
    CHECK_EQ(values[0], 10.0);
    CHECK_EQ(values[1], 20.0);
    CHECK_EQ(values[2], 30.0);
    CHECK_EQ(values[3], 40.0);
    CHECK_EQ(sample.quantile(0.0), 10.0);
    CHECK_EQ(sample.quantile(1.0), 40.0);
    CHECK_EQ(sample.quantile(0.5), 25.0);

    tally::Reservoir growing(8, 42);
    for (int i = 0; i < 100; ++i) {
        growing.add(static_cast<double>(i));
    }
    auto resumed = tally::Reservoir::deserialize(growing.serialize());
    CHECK(resumed == growing);
    for (int i = 100; i < 180; ++i) {
        growing.add(static_cast<double>(i));
        resumed.add(static_cast<double>(i));
    }
    CHECK(resumed == growing);
    CHECK_EQ(growing.size(), static_cast<std::size_t>(8));
    CHECK_EQ(growing.seen(), static_cast<std::uint64_t>(180));

    tally::Reservoir uniform(10'000, 7);
    constexpr int kStream = 100'000;
    for (int i = 0; i < kStream; ++i) {
        uniform.add(static_cast<double>(i));
    }
    CHECK_NEAR(uniform.quantile(0.5), (kStream - 1) / 2.0, 5000.0);

    bool rejected = false;
    try {
        tally::Reservoir empty(4, 1);
        (void)empty.quantile(0.5);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);

    bool non_finite = false;
    try {
        sample.add(std::numeric_limits<double>::infinity());
    } catch (const std::invalid_argument&) {
        non_finite = true;
    }
    CHECK(non_finite);
}

void test_space_saving() {
    tally::SpaceSaving exact(3);
    exact.add("a", 5);
    exact.add("b");
    exact.add("b");
    exact.add("c", 4);
    CHECK(exact.monitored("a"));
    CHECK_EQ(exact.upper_bound("a"), static_cast<std::uint64_t>(5));
    CHECK_EQ(exact.lower_bound("a"), static_cast<std::uint64_t>(5));
    CHECK_EQ(exact.upper_bound("b"), static_cast<std::uint64_t>(2));
    const auto rows = exact.top();
    CHECK_EQ(rows.size(), static_cast<std::size_t>(3));
    CHECK_EQ(rows[0].key, std::string("a"));
    CHECK_EQ(rows[0].count, static_cast<std::uint64_t>(5));

    tally::SpaceSaving summary(2);
    summary.add("a");
    summary.add("a");
    summary.add("a");
    summary.add("b");
    summary.add("c");
    CHECK(summary.monitored("a"));
    CHECK(!summary.monitored("b"));
    CHECK(summary.monitored("c"));
    CHECK_EQ(summary.upper_bound("a"), static_cast<std::uint64_t>(3));
    CHECK_EQ(summary.lower_bound("a"), static_cast<std::uint64_t>(3));
    CHECK_EQ(summary.upper_bound("c"), static_cast<std::uint64_t>(2));
    CHECK_EQ(summary.lower_bound("c"), static_cast<std::uint64_t>(1));
    CHECK_EQ(summary.upper_bound("b"), static_cast<std::uint64_t>(2));
    CHECK_EQ(summary.lower_bound("b"), static_cast<std::uint64_t>(0));

    tally::SpaceSaving ties(2);
    ties.add("a");
    ties.add("b");
    ties.add("c");
    CHECK(!ties.monitored("a"));
    CHECK(ties.monitored("b"));
    CHECK(ties.monitored("c"));
    CHECK_EQ(ties.upper_bound("b"), static_cast<std::uint64_t>(1));
    CHECK_EQ(ties.upper_bound("c"), static_cast<std::uint64_t>(2));

    std::unordered_map<std::string, std::uint64_t> truth;
    tally::SpaceSaving stream(30);
    std::uint64_t total = 0;
    for (int i = 0; i < 2000; ++i) {
        const std::string key = key_at(i % 80);
        const std::uint64_t weight = static_cast<std::uint64_t>((i * 3) % 5 + 1);
        stream.add(key, weight);
        truth[key] += weight;
        total += weight;
    }
    std::uint64_t counter_sum = 0;
    for (const auto& row : stream.top()) {
        counter_sum += row.count;
        CHECK(row.error <= row.count);
        CHECK(stream.lower_bound(row.key) <= truth[row.key]);
        CHECK(stream.upper_bound(row.key) >= truth[row.key]);
    }
    CHECK_EQ(counter_sum, total);
    for (const auto& [key, count] : truth) {
        CHECK(stream.upper_bound(key) >= count);
        CHECK(stream.lower_bound(key) <= count);
    }

    const auto restored = tally::SpaceSaving::deserialize(stream.serialize());
    CHECK(restored == stream);

    tally::SpaceSaving saturated(1);
    saturated.add("a", std::numeric_limits<std::uint64_t>::max());
    saturated.add("a", 1);
    CHECK_EQ(saturated.upper_bound("a"), std::numeric_limits<std::uint64_t>::max());
}

void run(const char* name, void (*fn)()) {
    try {
        fn();
    } catch (const std::exception& ex) {
        ++g_failed;
        std::cerr << name << " threw " << ex.what() << "\n";
    }
}

}  // namespace

int main() {
    run("hash", test_hash);
    run("bloom", test_bloom);
    run("hyperloglog", test_hyperloglog);
    run("count-min", test_count_min);
    run("reservoir", test_reservoir);
    run("space-saving", test_space_saving);
    std::cout << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
