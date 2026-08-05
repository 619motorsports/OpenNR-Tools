#include "byte_reader.h"

#include <cstring>

namespace opennr {

void ByteReader::ensure(std::size_t n) const {
    if (pos_ + n > bytes_.size()) {
        throw std::runtime_error(
            "ByteReader: read past end of buffer (need " + std::to_string(n) +
            " B at pos " + std::to_string(pos_) +
            ", size " + std::to_string(bytes_.size()) + ")");
    }
}

void ByteReader::seek(std::size_t pos) {
    if (pos > bytes_.size()) {
        throw std::runtime_error("ByteReader: seek past end of buffer");
    }
    pos_ = pos;
}

void ByteReader::skip(std::size_t n) {
    ensure(n);
    pos_ += n;
}

std::uint8_t ByteReader::read_u8() {
    ensure(1);
    return bytes_[pos_++];
}

std::uint16_t ByteReader::read_u16_le() {
    ensure(2);
    std::uint16_t v = static_cast<std::uint16_t>(bytes_[pos_]) |
                      (static_cast<std::uint16_t>(bytes_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
}

std::uint32_t ByteReader::read_u32_le() {
    ensure(4);
    std::uint32_t v = static_cast<std::uint32_t>(bytes_[pos_]) |
                      (static_cast<std::uint32_t>(bytes_[pos_ + 1]) << 8) |
                      (static_cast<std::uint32_t>(bytes_[pos_ + 2]) << 16) |
                      (static_cast<std::uint32_t>(bytes_[pos_ + 3]) << 24);
    pos_ += 4;
    return v;
}

std::int32_t ByteReader::read_i32_le() {
    return static_cast<std::int32_t>(read_u32_le());
}

float ByteReader::read_f32_le() {
    std::uint32_t bits = read_u32_le();
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

std::string ByteReader::read_pascal_then_null() {
    std::uint8_t len = read_u8();
    ensure(static_cast<std::size_t>(len) + 1);
    std::string name(reinterpret_cast<const char*>(&bytes_[pos_]), len);
    pos_ += len;
    std::uint8_t nul = bytes_[pos_++];
    if (nul != 0) {
        throw std::runtime_error("ByteReader: expected NUL terminator after name");
    }
    return name;
}

std::span<const std::uint8_t> ByteReader::read_bytes(std::size_t n) {
    ensure(n);
    auto sub = bytes_.subspan(pos_, n);
    pos_ += n;
    return sub;
}

std::string_view ByteReader::peek_fourcc() const {
    if (pos_ + 4 > bytes_.size()) {
        return {};
    }
    return std::string_view(reinterpret_cast<const char*>(&bytes_[pos_]), 4);
}

}  // namespace opennr
