#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace opennr {

// Decoded structure of a Papyrus .stp ("STMP") image file.
//
// See docs/formats/stp_image.md for the on-disk format. Container parses
// here; pixel-format decoding is handled together with .mip in a later
// pass once the format-byte → D3DFORMAT switch is pinned down.
//
// One .stp file holds a single BMAP image (one combined width x height
// surface) plus an outer table of per-sub-image dimensions when the file
// stores a strip (STHD count > 1).
struct StpSubImage {
    std::uint32_t width  = 0;   // dimensions of this slice in the strip
    std::uint32_t height = 0;
};

struct StpImage {
    std::uint32_t              sthd_count = 0;
    std::vector<StpSubImage>   subimages;     // size == sthd_count

    std::uint8_t  format    = 0;   // pixel-format enum (see docs)
    std::uint32_t width     = 0;   // BMAP width
    std::uint32_t height    = 0;   // BMAP height
    std::uint32_t pitch     = 0;   // bytes per row, 4-byte aligned
    std::uint8_t  n_mips    = 0;   // 0 = raw payload, 1 = DCL-compressed

    // Raw decoded pixel bytes. When n_mips=0 these are taken directly
    // from the on-disk DATA payload; when n_mips=1 they are produced by
    // running DCL "blast" decompression over the payload. Size always
    // equals pitch * height.
    std::vector<std::uint8_t>  pixels;

    static StpImage parse(std::span<const std::uint8_t> bytes);
};

}  // namespace opennr
