#include "fs/papyrus_piff.h"

#include <stdexcept>

namespace opennr::papyrus {

PiffReader::PiffReader(std::span<const std::uint8_t> bytes)
    : reader_(bytes), bytes_(bytes) {}

std::optional<PiffHeader> PiffReader::read_header() {
    if (reader_.remaining() == 0) return std::nullopt;
    if (reader_.remaining() < 12) {
        throw std::runtime_error("papyrus::PiffReader: truncated chunk header");
    }
    PiffHeader h;
    h.fourcc       = reader_.read_u32_le();
    h.version      = reader_.read_u32_le();
    h.payload_size = reader_.read_i32_le();
    return h;
}

std::span<const std::uint8_t> PiffReader::read_payload(const PiffHeader& h) {
    if (h.payload_size < 0) {
        throw std::runtime_error("papyrus::PiffReader: negative payload size");
    }
    auto raw = reader_.read_bytes(static_cast<std::size_t>(h.payload_size));
    std::size_t padded = piff_payload_padded(h.payload_size);
    std::size_t pad    = padded - static_cast<std::size_t>(h.payload_size);
    if (pad) reader_.skip(pad);
    return raw;
}

void PiffReader::skip_payload(const PiffHeader& h) {
    if (h.payload_size < 0) {
        throw std::runtime_error("papyrus::PiffReader: negative payload size");
    }
    reader_.skip(piff_payload_padded(h.payload_size));
}

}  // namespace opennr::papyrus
