#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace opennr {

// Canonical in-memory representation used by Opponent Manager's texture
// import/export path. Pixels are top-to-bottom, left-to-right RGBA8888.
struct TgaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;

    // Papyrus' reader accepts 15/16/24/32-bit true-colour TGA data in raw or
    // RLE form and honours both origin bits. Colour-mapped/grayscale images
    // are not accepted by the Opponent Manager texture builder.
    static TgaImage parse(std::span<const std::uint8_t> bytes);

    // Papyrus' export helper always emits an uncompressed, top-left-origin,
    // 24-bit TGA. Alpha is intentionally discarded.
    std::vector<std::uint8_t> serialize_24bit_top_left() const;
};

}  // namespace opennr
