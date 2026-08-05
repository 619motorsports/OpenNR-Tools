#include "bff_font.h"

#include "dcl_blast.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace opennr {

namespace {

// FourCCs as little-endian DWORDs.  On disk the bytes are read in
// reverse order ("TNFP" == DWORD 'PFNT').
// Magic constants are the LE-u32 reading of the on-disk byte-reversed
// FourCCs. The on-disk bytes for the logical FourCC "PFNT" are
// `T N F P` = `0x54 0x4E 0x46 0x50` (reversed-FourCC convention shared
// with .mip / .stp / .sim / .cam). Read those 4 bytes as a little-
// endian u32 and you get `0x50464E54` — the constants below match
// that LE-u32 form so a direct memcpy-into-u32 comparison works.
constexpr std::uint32_t kPFNT = 0x50464E54u;  // bytes T N F P
constexpr std::uint32_t kPFHD = 0x50464844u;  // bytes D H F P
constexpr std::uint32_t kPFGD = 0x50464744u;  // bytes D G F P
constexpr std::uint32_t kSTMP = 0x53544D50u;  // bytes P M T S
constexpr std::uint32_t kSTHD = 0x53544844u;  // bytes D H T S
constexpr std::uint32_t kBMAP = 0x424D4150u;  // bytes P A M B
constexpr std::uint32_t kBMHD = 0x424D4844u;  // bytes D H M B
constexpr std::uint32_t kDATA = 0x44415441u;  // bytes A T A D

std::uint16_t read_u16_le(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t read_u32_le(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

float read_f32_le(const std::uint8_t *p) {
    std::uint32_t bits = read_u32_le(p);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

// IEEE 754 binary16 (half precision) to float32.  BFF stores
// fractional pixel metrics (left bearing, kerning delta) as
// half-floats padded with two zero bytes to a 4-byte slot.
float half_to_float(std::uint16_t h) {
    std::uint32_t sign = (h >> 15) & 0x1u;
    std::uint32_t exp  = (h >> 10) & 0x1fu;
    std::uint32_t mant = h & 0x3ffu;
    std::uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign << 31;
        } else {
            // Subnormal half - renormalize.
            std::uint32_t e = 127 - 14;
            while ((mant & 0x400u) == 0) { mant <<= 1; --e; }
            mant &= 0x3ffu;
            bits = (sign << 31) | (e << 23) | (mant << 13);
        }
    } else if (exp == 0x1fu) {
        bits = (sign << 31) | (0xffu << 23) | (mant << 13);
    } else {
        bits = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

// Chunk header layout: u32 magic, u32 reserved, u32 payload_size.
struct Chunk {
    std::uint32_t magic        = 0;
    std::size_t   payload_off  = 0;
    std::size_t   payload_size = 0;
};

// Reads a 12-byte chunk header at `off` and returns the payload view.
// `off` is advanced past the header.
Chunk read_chunk_header(std::span<const std::uint8_t> bytes, std::size_t &off) {
    if (off + 12 > bytes.size()) {
        throw std::runtime_error("BFF: chunk header runs past EOF");
    }
    Chunk c;
    c.magic        = read_u32_le(&bytes[off]);
    // skip 4 reserved bytes between magic and size
    c.payload_size = read_u32_le(&bytes[off + 8]);
    c.payload_off  = off + 12;
    if (c.payload_off + c.payload_size > bytes.size()) {
        throw std::runtime_error("BFF: chunk payload runs past EOF");
    }
    off = c.payload_off + c.payload_size;
    // Pad up to 4-byte alignment.
    while ((off & 3) != 0 && off < bytes.size() && bytes[off] == 0x20) {
        ++off;
    }
    return c;
}

}  // namespace

const BffGlyph *BffFont::find_glyph(std::uint32_t cp) const {
    for (const auto &g : glyphs) {
        if (g.codepoint == cp) return &g;
    }
    return nullptr;
}

BffFont BffFont::parse(std::span<const std::uint8_t> bytes) {
    BffFont out;

    // ---- outer PFNT --------------------------------------------------
    if (bytes.size() < 12) throw std::runtime_error("BFF: file shorter than header");
    std::size_t off = 0;
    Chunk root = read_chunk_header(bytes, off);
    if (root.magic != kPFNT) throw std::runtime_error("BFF: not a PFNT file");

    std::span<const std::uint8_t> body = bytes.subspan(root.payload_off, root.payload_size);

    // Children of PFNT.
    std::size_t coff = 0;
    Chunk pfhd{}, pfgd{}, stmp{}, bmap{};
    while (coff < body.size()) {
        Chunk c = read_chunk_header(body, coff);
        switch (c.magic) {
            case kPFHD: pfhd = c; break;
            case kPFGD: pfgd = c; break;
            case kSTMP: stmp = c; break;
            case kBMAP: bmap = c; break;
            default: /* skip unknown sibling */ break;
        }
    }
    if (pfhd.magic == 0 || pfgd.magic == 0 || stmp.magic == 0) {
        throw std::runtime_error("BFF: missing one of PFHD/PFGD/STMP");
    }
    // BMAP may be a direct child of PFNT OR nested under STMP; we
    // discover the nested case after we walk STMP below.

    // ---- PFHD (font header, 6+ bytes) -------------------------------
    if (pfhd.payload_size < 6) throw std::runtime_error("BFF: PFHD too short");
    const std::uint8_t *h = &body[pfhd.payload_off];
    out.line_height_px              = read_u16_le(h + 0);
    out.descent_below_baseline_px   = read_u16_le(h + 2);
    out.pfhd_flags                  = read_u16_le(h + 4);

    // ---- PFGD (glyph descriptors) -----------------------------------
    //
    // PFGD body layout (verified against NR2003.exe FUN_005f9bb0):
    //   u16 glyph_count                 <-- 2-byte header
    //   per glyph (16 fixed bytes + u16 kerning_count + kc * 6 bytes):
    //     p[ 0.. 1]  u16  codepoint
    //     p[ 2.. 5]  f32  left side bearing in pixels  (unaligned read)
    //     p[ 6.. 9]  f32  pen advance in pixels        (unaligned read)
    //     p[10..11]  u16  reserved (zero in shipped faces)
    //     p[12..13]  u16  per-glyph extent metric (cap-height for caps,
    //                     x-height for x-height letters, descender-extent
    //                     for low punctuation; not used at draw time)
    //     p[14..15]  s16  bitmap bottom relative to baseline; negative =
    //                     sits above baseline, positive = descends below.
    //     p[16..17]  u16  kerning_count
    //
    // The first descriptor is the space glyph (cp = 0x20), whose
    // p[14..15] also doubles as a "version=3" sentinel — read it as a
    // s16 anyway, since space has no bitmap and y_descent is ignored.
    if (pfgd.payload_size < 2) throw std::runtime_error("BFF: PFGD too short");
    const std::uint8_t *gptr = &body[pfgd.payload_off];
    std::uint16_t pfgd_glyph_count = read_u16_le(gptr + 0);

    std::size_t gpos = 2;  // first descriptor starts right after the count
    std::size_t gend = pfgd.payload_size;
    while (gpos + 16 <= gend && out.glyphs.size() < pfgd_glyph_count) {
        BffGlyph g;
        const std::uint8_t *p = gptr + gpos;
        g.codepoint    = read_u16_le(p + 0);
        g.bearing_px   = read_f32_le(p + 2);
        g.advance_px   = read_f32_le(p + 6);
        std::uint16_t descent_raw = read_u16_le(p + 14);
        g.y_descent_px = static_cast<std::int16_t>(descent_raw);
        gpos += 16;

        // u16 kerning_count, then kc * (u16 next_cp, f32 delta).
        if (gpos + 2 > gend) break;
        std::uint16_t kc = read_u16_le(gptr + gpos);
        gpos += 2;
        for (std::uint16_t k = 0; k < kc; ++k) {
            if (gpos + 6 > gend) break;
            BffKerningPair kp;
            kp.next_codepoint = read_u16_le(gptr + gpos);
            kp.delta_px       = read_f32_le(gptr + gpos + 2);
            g.kerning.push_back(kp);
            gpos += 6;
        }
        out.glyphs.push_back(std::move(g));
        if (pfgd_glyph_count > 0 && out.glyphs.size() >= pfgd_glyph_count) break;
    }

    // Space carries its own descriptor (cp 0x20, first entry); STMP's
    // slot 0 is just a placeholder rect with advance=0.  Look the value
    // up so the legacy synthesize-from-slot path below can fall back to
    // it if STMP runs ahead of PFGD.
    float pfgd_space_advance = 0.f;
    if (const auto *sp = out.find_glyph(0x20)) {
        pfgd_space_advance = sp->advance_px;
    }

    // ---- STMP (atlas slot table) ------------------------------------
    // STMP itself is a small wrapper containing STHD + DATA sub-chunks,
    // and in shipped fonts it also nests the BMAP bitmap chunk (the
    // outer-level enumeration above only finds PFHD/PFGD/STMP as direct
    // children of PFNT; BMAP is one level deeper).
    std::span<const std::uint8_t> sbody = body.subspan(stmp.payload_off, stmp.payload_size);
    std::size_t scoff = 0;
    std::uint16_t stmp_glyph_count = 0;
    std::span<const std::uint8_t> stmp_data;
    while (scoff < sbody.size()) {
        Chunk c = read_chunk_header(sbody, scoff);
        if (c.magic == kSTHD) {
            if (c.payload_size >= 4) {
                const std::uint8_t *p = &sbody[c.payload_off];
                stmp_glyph_count = read_u16_le(p + 0);
            }
        } else if (c.magic == kDATA && stmp_data.empty()) {
            // First DATA inside STMP is the slot table; subsequent DATA
            // (inside BMAP) is the compressed bitmap.  Disambiguate by
            // "first DATA only".
            stmp_data = sbody.subspan(c.payload_off, c.payload_size);
        } else if (c.magic == kBMAP && bmap.magic == 0) {
            // BMAP nested under STMP - the layout shipped fonts use.
            bmap.magic        = c.magic;
            bmap.payload_off  = stmp.payload_off + c.payload_off;
            bmap.payload_size = c.payload_size;
        }
    }
    if (bmap.magic == 0) {
        // No BMAP found inside STMP either — the file is malformed.
        throw std::runtime_error("BFF: missing BMAP chunk");
    }

    // The slot table is one 12-byte record per glyph in the font's
    // codepoint sequence; index 0 is the first encoded codepoint
    // (always space, 0x20, in shipped faces).  Slot rectangles are
    // copied onto the descriptor that matches by codepoint; a synthetic
    // glyph is inserted for slots without a PFGD entry (rare).
    if (stmp_data.size() < std::size_t(stmp_glyph_count) * 12) {
        // Don't throw; just clamp to whatever is there.
        stmp_glyph_count = static_cast<std::uint16_t>(stmp_data.size() / 12);
    }
    const std::uint16_t stmp_first_cp = 0x20;
    for (std::uint16_t i = 0; i < stmp_glyph_count; ++i) {
        const std::uint8_t *p = &stmp_data[std::size_t(i) * 12];
        // Slot record format (12 bytes; vertical-strip atlas):
        //   p[0..1]   u16 reserved (zero)
        //   p[2..3]   u16 atlas_y  — first row of this glyph in the strip
        //   p[4..5]   u16 glyph_w  — bitmap width in pixels
        //   p[6..7]   u16 glyph_h  — bitmap height (row count)
        //   p[8..11]  u32 trailing — high u16 == pen-advance in pixels
        std::uint16_t atlas_y = read_u16_le(p + 2);
        std::uint16_t glyph_w = read_u16_le(p + 4);
        std::uint16_t glyph_h = read_u16_le(p + 6);
        std::uint32_t trail   = static_cast<std::uint32_t>(read_u16_le(p + 8)) |
                                (static_cast<std::uint32_t>(read_u16_le(p + 10)) << 16);
        std::uint16_t slot_adv = static_cast<std::uint16_t>((trail >> 16) & 0xFFFF);

        std::uint16_t cp = static_cast<std::uint16_t>(stmp_first_cp + i);
        BffGlyph *match = nullptr;
        for (auto &g : out.glyphs) {
            if (g.codepoint == cp) { match = &g; break; }
        }
        if (match == nullptr) {
            // Slot without a PFGD entry — synthesize.
            BffGlyph g;
            g.codepoint  = cp;
            g.atlas_x    = 0;
            g.atlas_y    = atlas_y;
            g.atlas_w    = glyph_w;
            g.atlas_h    = glyph_h;
            g.width_px   = glyph_w;
            float adv;
            if (cp == 0x20 && pfgd_space_advance > 0.f) {
                adv = pfgd_space_advance;
            } else if (slot_adv > 0) {
                adv = float(slot_adv);
            } else if (glyph_w > 0) {
                adv = float(glyph_w);
            } else {
                adv = float(out.line_height_px) * 0.25f;
            }
            g.advance_px = adv;
            out.glyphs.push_back(g);
        } else {
            match->atlas_x = 0;
            match->atlas_y = atlas_y;
            match->atlas_w = glyph_w;
            match->atlas_h = glyph_h;
            if (match->width_px == 0) match->width_px = glyph_w;
            // Slot trail's advance is a fallback only — the PFGD f32
            // advance is the authoritative pen-advance and is set on
            // every shipped glyph (including space).
            if (match->advance_px == 0.f && slot_adv > 0) {
                match->advance_px = float(slot_adv);
            }
        }
    }

    // ---- BMAP (compressed atlas) -----------------------------------
    //
    // Atlas layout: a *vertical* strip, 8 bits per pixel (alpha
    // 0..255).  BMHD encodes the dimensions:
    //   byte 0: 0x09 (format tag)
    //   byte 1: logical atlas width in pixels (= max glyph_w)
    //   bytes 5..7: atlas_h in pixels (u24 LE)
    //   byte 9: row stride in bytes — equals byte 1 when the width is a
    //           multiple of 4, otherwise rounded up to the next 4-byte
    //           boundary.  Each decompressed row is `stride` bytes; the
    //           trailing bytes beyond the logical width are pad and stay
    //           zero, so we just treat the texture as `stride` wide.
    //   byte 13: bytes-per-pixel (always 1 in shipped faces).
    std::span<const std::uint8_t> bbody = body.subspan(bmap.payload_off, bmap.payload_size);
    std::size_t bcoff = 0;
    std::span<const std::uint8_t> bmap_data;
    std::uint32_t atlas_w = 0;
    std::uint32_t atlas_h = 0;
    while (bcoff < bbody.size()) {
        Chunk c = read_chunk_header(bbody, bcoff);
        if (c.magic == kBMHD && c.payload_size >= 10) {
            const std::uint8_t *bh = &bbody[c.payload_off];
            std::uint32_t logical_w = bh[1];
            std::uint32_t stride    = bh[9];
            // Use the larger of the two so the row layout matches the
            // decompressed data; logical_w == stride for fonts whose
            // width is already 4-aligned.
            atlas_w = (stride >= logical_w) ? stride : logical_w;
            atlas_h = static_cast<std::uint32_t>(bh[5]) |
                      (static_cast<std::uint32_t>(bh[6]) << 8)  |
                      (static_cast<std::uint32_t>(bh[7]) << 16);
        } else if (c.magic == kDATA) {
            bmap_data = bbody.subspan(c.payload_off, c.payload_size);
        }
    }

    // Fall back to bounds computed from slot rects when BMHD is
    // missing or malformed.
    if (atlas_w == 0 || atlas_h == 0) {
        for (const auto &g : out.glyphs) {
            atlas_w = std::max<std::uint32_t>(atlas_w, g.atlas_w);
            atlas_h = std::max<std::uint32_t>(atlas_h,
                std::uint32_t(g.atlas_y) + g.atlas_h);
        }
        if (atlas_w == 0) atlas_w = 8;
        if (atlas_h == 0) atlas_h = 1;
    }

    out.atlas_w = atlas_w;
    out.atlas_h = atlas_h;
    out.atlas_alpha.assign(std::size_t(atlas_w) * atlas_h, 0);

    if (!bmap_data.empty()) {
        std::vector<std::uint8_t> raw;
        try {
            raw = dcl_decompress(bmap_data);
        } catch (...) {
            return out;
        }
        // 8bpp row-major: byte[y * atlas_w + x] == alpha at (x, y).
        std::size_t copy_n = std::min(raw.size(),
            std::size_t(atlas_w) * atlas_h);
        std::memcpy(out.atlas_alpha.data(), raw.data(), copy_n);
    }

    return out;
}

}  // namespace opennr
