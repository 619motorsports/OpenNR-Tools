#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace opennr {

// Lightweight bounds-checked little-endian reader over a borrowed byte buffer.
// Throws std::runtime_error on out-of-range access so callers don't have to
// scatter checks.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> bytes) noexcept
        : bytes_(bytes), pos_(0) {}

    std::size_t position() const noexcept { return pos_; }
    std::size_t size() const noexcept { return bytes_.size(); }
    std::size_t remaining() const noexcept { return bytes_.size() - pos_; }

    void seek(std::size_t pos);
    void skip(std::size_t n);

    std::uint8_t  read_u8();
    std::uint16_t read_u16_le();
    std::uint32_t read_u32_le();
    std::int32_t  read_i32_le();
    float         read_f32_le();

    // Read a length-prefixed (1-byte length, no embedded NUL) ASCII name, then
    // consume the trailing NUL byte. Used by the DAT TOC.
    std::string read_pascal_then_null();

    // Read raw bytes; returns a view into the underlying buffer.
    std::span<const std::uint8_t> read_bytes(std::size_t n);

    // Peek (does not advance) a 4-character FourCC starting at the current pos.
    std::string_view peek_fourcc() const;

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t pos_;

    void ensure(std::size_t n) const;
};

}  // namespace opennr
