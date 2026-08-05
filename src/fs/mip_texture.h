#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace opennr {

// Decoded structure of a Papyrus .mip texture file.
//
// See docs/formats/mip_texture.md for the on-disk format. The file is a
// 32-byte file header followed by one or more BMAP chunks (smallest mip
// first). Each BMAP holds a BMHD (per-level format / width / height /
// pitch / n_mips) plus a DATA payload that is either raw pixels or
// DCL-compressed pixels.
//
// FourCCs in .mip files are byte-reversed on disk just like .stp / .sim
// / .cam — the on-disk bytes for "BMAP" are 'P','A','M','B'.

// Type-byte values observed in BMHD across the shipped install (and
// documented in WinMip2's bundled help for NR2002/3):
//
//   3  -> 16-bit RGB565            (5R 6G 5B,        opaque)
//   4  -> 16-bit RGB555 + colorkey (5R 5G 5B,        invisible-color)
//   5  -> 16-bit ARGB4444          (4A 4R 4G 4B)
//   6  -> 24-bit RGB888            (3 bytes/pixel,   opaque)        [stp/loose]
//   7  -> 32-bit BGRA8888          (4 bytes/pixel,   8-bit alpha)
//   10 -> DXT1                     (S3TC, 0.5 byte/pixel)
//   11 -> DXT3                     (S3TC, 1 byte/pixel + 4-bit alpha)
//
// Format byte values 0x00 / 0x01 / 0x02 also appear in the FILE header's
// byte 3, but those are content-category tags, not pixel format.
// Authoritative format is the BMHD byte 0 inside each level.
enum : std::uint8_t {
    kMipFormatRGB565    = 0x03,
    kMipFormatRGB555Key = 0x04,
    kMipFormatARGB4444  = 0x05,
    kMipFormatRGB888    = 0x06,
    kMipFormatBGRA8888  = 0x07,
    kMipFormatDXT1      = 0x0A,
    kMipFormatDXT3      = 0x0B,
};

struct MipLevel {
    std::uint8_t  format = 0;       // BMHD byte 0
    std::uint32_t width  = 0;       // pixel width
    std::uint32_t height = 0;       // pixel height
    std::uint32_t pitch  = 0;       // bytes per row, 4-byte aligned
    std::uint8_t  n_mips = 0;       // BMHD byte 13: 0 = raw, 1 = DCL
    std::vector<std::uint8_t> pixels;  // size always == pitch * height
                                       // (DCL-decompressed when n_mips=1)
};

struct MipTexture {
    // Exact 32-byte file header written by FUN_005e92f0.
    std::uint8_t  sig0          = 0;     // observed 0x04
    std::uint8_t  sig1          = 0;     // observed 0x00
    std::uint8_t  format        = 0;     // base BMHD pixel format
    std::uint8_t  header_byte_3 = 0;     // bit0 U clamp, bit1 V clamp
    std::uint32_t field4        = 0;     // log2(base width)
    std::uint32_t field8        = 0;     // log2(base height)
    std::uint32_t color_key     = 0;     // 0xAARRGGBB key/sample colour;
                                         // format 4 uses it as the key
    std::uint32_t mip_count     = 0;
    std::uint32_t field20       = 0;
    float         factor        = 0.0f;  // authored ~0.9; imports use 0.8
    std::uint32_t field28       = 0;
    std::vector<MipLevel> levels;        // smallest level first

    bool clamp_u() const { return (header_byte_3 & 0x01u) != 0; }
    bool clamp_v() const { return (header_byte_3 & 0x02u) != 0; }

    static MipTexture parse(std::span<const std::uint8_t> bytes);

    // Reproduce the original imported-pixel texture builder: power-of-two
    // dimensions from 1 through 4096, RGB888 when every alpha byte is 0xff
    // and BGRA8888 otherwise, both UV axes clamped, and a complete mip chain.
    // Input is top-down RGBA8888. Opponent Manager's TGA reader supplies an
    // opaque RGB888 intermediate, so that call site always takes RGB888.
    static MipTexture from_rgba8888(
        std::uint32_t width, std::uint32_t height,
        std::span<const std::uint8_t> rgba);

    // Write the 32-byte header and smallest-first BMAP/BMHD/DATA stream.
    // When requested, use the retail DCL writer policy: retain compression
    // only when the compressed payload is strictly less than half the raw
    // payload; otherwise retain raw DATA.
    std::vector<std::uint8_t> serialize(bool compress_data = false) const;
};

// Decode one level's pixels into RGBA8888 (one byte per channel, R-G-B-A
// order in memory). Output size is `width * height * 4`.
//
// Supports every format used by shipped NR2003 .mip files:
//   RGB565, RGB555+colorkey, ARGB4444, RGB888, BGRA8888, DXT1, DXT3.
std::vector<std::uint8_t> mip_decode_to_rgba8888(
    const MipLevel& lvl,
    std::uint32_t   color_key = 0);

}  // namespace opennr
