#include "descriptors.h"

#include <cstring>

namespace opennr {

namespace {

std::uint32_t read_u32(std::span<const std::uint8_t> b, std::size_t p) {
    if (p + 4 > b.size()) return 0;
    return std::uint32_t(b[p]) |
           (std::uint32_t(b[p + 1]) <<  8) |
           (std::uint32_t(b[p + 2]) << 16) |
           (std::uint32_t(b[p + 3]) << 24);
}

}  // namespace

DescriptorHeader decode_descriptor_header(
    std::span<const std::uint8_t> bytes, std::size_t body_pos) {
    DescriptorHeader h;
    if (body_pos + 8 > bytes.size()) {
        h.header_bytes = bytes.size() - body_pos;
        return h;
    }
    h.version     = read_u32(bytes, body_pos);
    h.name_length = read_u32(bytes, body_pos + 4);
    h.header_bytes = 8;
    // Sanity: name_length must be plausible; if not we don't consume it.
    if (h.name_length > 0 && h.name_length <= 256 &&
        body_pos + 8 + h.name_length <= bytes.size() &&
        bytes[body_pos + 8 + h.name_length - 1] == 0) {
        // Validate the bytes are all printable ASCII before treating it
        // as a name; this guards against false-positives on descriptors
        // whose second u32 happens to be small but isn't a name length.
        bool ok = true;
        for (std::uint32_t i = 0; i < h.name_length - 1; ++i) {
            std::uint8_t b = bytes[body_pos + 8 + i];
            if (b < 0x20 || b >= 0x7F) { ok = false; break; }
        }
        if (ok) {
            h.name.assign(
                reinterpret_cast<const char *>(&bytes[body_pos + 8]),
                h.name_length - 1);
            h.header_bytes = 8 + h.name_length;
        } else {
            h.name_length = 0;
        }
    } else {
        h.name_length = 0;
    }
    return h;
}

}  // namespace opennr
