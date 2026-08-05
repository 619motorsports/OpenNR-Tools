#pragma once

// Papyrus `.bff` font parser.
//
// `.bff` files are pre-rasterized bitmap fonts: a list of per-glyph
// metrics (advance, width, optional kerning pairs) plus an 8-bit alpha
// atlas of glyph bitmaps that has been DCL-compressed.  See
// `docs/formats/bff_font.md` for the chunked container layout.
//
// This parser:
//   * walks the FourCC chunk tree (PFNT > {PFHD, PFGD, STMP, BMAP})
//   * pulls per-glyph metrics out of PFGD (including kerning sub-records)
//   * pulls per-glyph atlas rectangles out of STMP
//   * DCL-decompresses the BMAP/DATA bitstream into an 8-bit alpha image
//     ready for an alpha-blended quad.
//
// The output `BffFont::atlas_alpha` is row-major 8-bit (0 = ground,
// 255 = ink); upload it to a texture with `PixelFormat::a8` (or expand
// to RGBA in your texture builder) and key per-glyph blits off
// `find_glyph(codepoint)->atlas_x/atlas_y/width/height`.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace opennr {

struct BffKerningPair {
    std::uint16_t next_codepoint = 0;
    float         delta_px       = 0.0f;
};

struct BffGlyph {
    std::uint16_t                codepoint  = 0;
    float                        bearing_px = 0.0f;  // left-side bearing: f32 at PFGD+2 (unaligned 4-byte read)
    float                        advance_px = 0.0f;  // pen advance:       f32 at PFGD+6 (unaligned 4-byte read)
    std::int16_t                 y_descent_px = 0;   // s16 at PFGD+14: bitmap bottom relative to baseline. Negative = sits above baseline (gap), positive = descender below baseline
    std::uint16_t                width_px   = 0;     // mirrors atlas_w after STMP merge (kept for legacy callers)
    std::uint16_t                atlas_x    = 0;
    std::uint16_t                atlas_y    = 0;
    std::uint16_t                atlas_w    = 0;
    std::uint16_t                atlas_h    = 0;
    std::vector<BffKerningPair>  kerning;
};

struct BffFont {
    // Font-level metrics (from PFHD).
    std::uint16_t                line_height_px = 0;
    // PFHD u16@2: descent below baseline in pixels (max gap from
    // baseline to bottom-of-line for a descender). baseline within a
    // line = line_height_px - descent_below_baseline_px.
    std::uint16_t                descent_below_baseline_px = 0;
    std::uint16_t                pfhd_flags     = 0;

    // Per-glyph metrics + atlas slots, indexed by glyph order on disk
    // (same order as the STMP slot table).
    std::vector<BffGlyph>        glyphs;

    // 8-bit alpha atlas: width × height bytes, row-major, top-left
    // origin.  255 = ink, 0 = transparent.
    std::uint32_t                atlas_w        = 0;
    std::uint32_t                atlas_h        = 0;
    std::vector<std::uint8_t>    atlas_alpha;

    // Lookup helpers.
    const BffGlyph *find_glyph(std::uint32_t codepoint) const;

    // Parse a full `.bff` file from a byte view.  Throws
    // std::runtime_error on malformed input.
    static BffFont parse(std::span<const std::uint8_t> bytes);
};

}  // namespace opennr
