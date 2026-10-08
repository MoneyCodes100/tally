#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error tally requires a little-endian platform
#endif

namespace tally::detail {

inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::uint64_t kMaxBlobBytes = 64ull << 20;

inline void require(bool condition, const char* message) {
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

inline std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) noexcept {
    const std::uint64_t sum = a + b;
    return sum < a ? std::numeric_limits<std::uint64_t>::max() : sum;
}

// Lemire's nearly-divisionless map of a 64-bit hash onto [0, range).
inline std::size_t fastrange(std::uint64_t hash, std::size_t range) {
    require(range > 0, "range must be positive");
#if defined(__SIZEOF_INT128__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wpedantic"
    const auto high = static_cast<__uint128_t>(hash) * static_cast<__uint128_t>(range);
#    pragma GCC diagnostic pop
    return static_cast<std::size_t>(high >> 64);
#else
    return static_cast<std::size_t>(hash % range);
#endif
}

class ByteWriter {
public:
    void u8(std::uint8_t value) { out_.push_back(static_cast<char>(value)); }

    void u32(std::uint32_t value) { raw(&value, sizeof(value)); }

    void u64(std::uint64_t value) { raw(&value, sizeof(value)); }

    void f64(double value) { raw(&value, sizeof(value)); }

    void raw(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const char*>(data);
        out_.append(bytes, size);
    }

    void str(std::string_view text) {
        u64(static_cast<std::uint64_t>(text.size()));
        raw(text.data(), text.size());
    }

    std::string take() { return std::move(out_); }

private:
    std::string out_;
};

class ByteReader {
public:
    explicit ByteReader(std::string_view bytes) : in_(bytes) {}

    // Checks the 4-byte magic and the format version that follows it.
    void magic(char a, char b, char c, char d) {
        if (in_.size() < 8 || in_[0] != a || in_[1] != b || in_[2] != c || in_[3] != d) {
            throw std::invalid_argument("unrecognized tally snapshot");
        }
        i_ = 4;
        if (u32() != kFormatVersion) {
            throw std::invalid_argument("unsupported tally snapshot version");
        }
    }

    std::uint32_t u32() {
        std::uint32_t value = 0;
        read(&value, sizeof(value));
        return value;
    }

    std::uint64_t u64() {
        std::uint64_t value = 0;
        read(&value, sizeof(value));
        return value;
    }

    double f64() {
        double value = 0;
        read(&value, sizeof(value));
        return value;
    }

    std::string str() {
        const std::uint64_t size = u64();
        require(size <= kMaxBlobBytes, "tally snapshot field is too large");
        std::string text(static_cast<std::size_t>(size), '\0');
        read(text.data(), text.size());
        return text;
    }

    template <typename T>
    void vec(std::vector<T>& out, std::uint64_t count) {
        require(count <= kMaxBlobBytes / sizeof(T), "tally snapshot field is too large");
        out.resize(static_cast<std::size_t>(count));
        read(out.data(), out.size() * sizeof(T));
    }

    void raw(void* dest, std::size_t size) { read(dest, size); }

    void finish() const {
        if (i_ != in_.size()) {
            throw std::invalid_argument("trailing bytes in tally snapshot");
        }
    }

private:
    void read(void* dest, std::size_t size) {
        if (i_ > in_.size() || size > in_.size() - i_) {
            throw std::invalid_argument("truncated tally snapshot");
        }
        std::memcpy(dest, in_.data() + i_, size);
        i_ += size;
    }

    std::string_view in_;
    std::size_t i_ = 0;
};

}  // namespace tally::detail
