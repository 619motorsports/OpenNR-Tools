#include "lyt_layout.h"

#include "core/byte_reader.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace opennr {

namespace {

// Layout constants (see docs/formats/lyt_layout.md).
constexpr std::size_t kFileHeaderSize  = 16;
constexpr std::size_t kRecordSize      = 1137;

// Fixed offsets within a 1137-byte widget record.
constexpr std::size_t kOffName         = 0x000;  // 32 bytes
constexpr std::size_t kSizeName        = 32;
constexpr std::size_t kOffType         = 0x100;
constexpr std::size_t kOffX            = 0x104;
constexpr std::size_t kOffY            = 0x108;
constexpr std::size_t kOffWidth        = 0x10C;
constexpr std::size_t kOffHeight       = 0x110;
constexpr std::size_t kOffGroupId      = 0x114;

constexpr std::size_t kOffAnimFrameCount    = 0x13C;
constexpr std::size_t kOffAnimFrameDuration = 0x140;
constexpr std::size_t kOffAnimFrame0        = 0x150;  // 16 bytes
constexpr std::size_t kOffAnimFrame1        = 0x160;  // 16 bytes

// Image flags (verified 2026-07-11 against the type-9/25 ctor
// FUN_00613f50): 0x180 != 0 arms the animation driver after create,
// 0x181 != 0 selects the stretched/tiled sprite ctor that consumes
// (width, height); otherwise images draw at natural texture size.
constexpr std::size_t kOffImageAnimated = 0x180;  // u8 flag
constexpr std::size_t kOffImageStretch  = 0x181;  // u8 flag

constexpr std::size_t kOffSound1       = 0x182;  // 32-byte string in 37-byte slot
constexpr std::size_t kOffSound1Volume = 0x1A3;  // u32 percent (engine × 0.01)
constexpr std::size_t kOffSound2       = 0x1A7;  // ditto
constexpr std::size_t kOffSound2Volume = 0x1C8;  // u32 percent

constexpr std::size_t kOffColor0       = 0x1CC;  // 11-byte string in 33-byte slot
constexpr std::size_t kColorSlotStride = 33;
constexpr std::size_t kColorSlotCount  = 9;


// Binary ARGB colours after the 9 string slots (verified 2026-07-11:
// the type-6 rect ctor chain FUN_00613e10 → FUN_00615640 splats the
// dword at 0x2f9 into per-channel bytes; the type-16 listbox ctor
// FUN_00605dc0 passes both 0x2f5 and 0x2f9).  Every shipped type-6
// rect carries its fill here — the string slots are all empty.
constexpr std::size_t kOffFillAltArgb  = 0x2F5;  // u32 0xAARRGGBB
constexpr std::size_t kOffFillArgb     = 0x2F9;  // u32 0xAARRGGBB

constexpr std::size_t kOffFont1        = 0x305;  // 17-byte slot
constexpr std::size_t kOffFont2        = 0x316;  // 17-byte slot

constexpr std::size_t kOffTexturePath  = 0x32F;  // 33-byte slot
constexpr std::size_t kTableColumnStride = 5;
constexpr std::size_t kTableColumnMax    = 0x33;
constexpr std::size_t kOffWidgetArt    = 0x3B0;  // 33-byte slot (type-2 only)

// Caption + tooltip resource IDs (PapyRes.dll string table).
constexpr std::size_t kOffCaptionId    = 0x327;  // u32
constexpr std::size_t kOffTooltipId    = 0x32B;  // u32

// Trailing param block — confirmed by NR2003.exe disassembly
// (FUN_005fdcb0, FUN_0060bb60, FUN_0060bfd0, FUN_00600170).
constexpr std::size_t kOffAlignment    = 0x431;  // u32 {0,1,2}
constexpr std::size_t kOffParamMin     = 0x435;  // u32
constexpr std::size_t kOffParamMax     = 0x439;  // u32
constexpr std::size_t kOffParamDefault = 0x43D;  // u32
constexpr std::size_t kOffParamAxis    = 0x441;  // u32 {0,1,2}
// Auxiliary flag after the param block: on type-19 tables it's fed
// to the table's FUN_00608c80 setter; on buttons FUN_0060adf0 uses
// it as "draw with the pressed colour slot while active/selected".
constexpr std::size_t kOffParamAux     = 0x445;  // u32

// Two extra ASCII string slots used by button / radio constructors.
constexpr std::size_t kOffExtraStr1    = 0x449;  // 8-byte slot
constexpr std::size_t kOffExtraStr2    = 0x451;  // 28-byte slot
constexpr std::size_t kSizeExtraStr1   = 8;
constexpr std::size_t kSizeExtraStr2   = 28;

// Per-item value copied into every widget by the base ctor
// FUN_00609880 (record 0x459 → widget +0x199).  Nonzero only on
// type-2 radio/tab items in shipped data (values 1..3).
constexpr std::size_t kOffItemValue     = 0x459;  // u32

// Widget-tree + anchor system (i16s).
constexpr std::size_t kOffParentGroupId = 0x45D;  // i16
constexpr std::size_t kOffOwnGroupId    = 0x45F;  // i16
constexpr std::size_t kOffParentIndex   = 0x461;  // i16
constexpr std::size_t kOffAnchorCode    = 0x463;  // i16 (LytAnchor)

// Read up to `max_len` bytes starting at `off`, stopping at the first
// NUL.  Returns an empty string when the first byte is zero.
std::string read_fixed_string(std::span<const std::uint8_t> rec,
                              std::size_t off, std::size_t max_len) {
    if (off >= rec.size()) return {};
    std::size_t end = std::min(rec.size(), off + max_len);
    std::size_t n = 0;
    while (off + n < end && rec[off + n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(&rec[off]), n);
}

std::int32_t read_i32_at(std::span<const std::uint8_t> rec, std::size_t off) {
    if (off + 4 > rec.size()) return 0;
    std::uint32_t v = static_cast<std::uint32_t>(rec[off]) |
                      (static_cast<std::uint32_t>(rec[off + 1]) << 8) |
                      (static_cast<std::uint32_t>(rec[off + 2]) << 16) |
                      (static_cast<std::uint32_t>(rec[off + 3]) << 24);
    return static_cast<std::int32_t>(v);
}

std::uint32_t read_u32_at(std::span<const std::uint8_t> rec, std::size_t off) {
    return static_cast<std::uint32_t>(read_i32_at(rec, off));
}

std::int16_t read_i16_at(std::span<const std::uint8_t> rec, std::size_t off) {
    if (off + 2 > rec.size()) return 0;
    std::uint16_t v = static_cast<std::uint16_t>(rec[off]) |
                      (static_cast<std::uint16_t>(rec[off + 1]) << 8);
    return static_cast<std::int16_t>(v);
}

float read_f32_at(std::span<const std::uint8_t> rec, std::size_t off) {
    std::uint32_t bits = read_u32_at(rec, off);
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

bool ascii_is_xdigit(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

}  // namespace

std::string lyt_font_basename_for_widget(const LytWidget& w) {
    // Type 1 buttons store the caption face in slot 1.  When slot 1 is
    // empty (1 exception in shipped data) fall through to slot 0.
    if (w.type == 1) {
        if (!w.fonts[1].empty()) return w.fonts[1];
        return w.fonts[0];
    }
    if (!w.fonts[0].empty()) return w.fonts[0];
    return w.fonts[1];
}

std::optional<std::uint32_t>
parse_lyt_color_literal(std::string_view s) {
    // Empty / unset slot.
    if (s.empty()) return std::nullopt;
    // Canonical form is exactly 10 chars: "0x" + 8 hex digits.
    if (s.size() != 10) return std::nullopt;
    if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return std::nullopt;
    std::uint32_t value = 0;
    for (std::size_t i = 2; i < 10; ++i) {
        char c = s[i];
        if (!ascii_is_xdigit(c)) return std::nullopt;
        std::uint32_t nib = (c <= '9') ? (c - '0')
                          : (c <= 'F') ? (c - 'A' + 10)
                                       : (c - 'a' + 10);
        value = (value << 4) | nib;
    }
    return value;
}

LytLayout LytLayout::parse(std::span<const std::uint8_t> bytes, bool keep_raw) {
    if (bytes.size() < kFileHeaderSize) {
        throw std::runtime_error("LytLayout: file shorter than header");
    }

    LytLayout out;
    ByteReader r(bytes);
    out.version     = r.read_u32_le();
    out.record_size = r.read_u32_le();
    r.skip(8);  // reserved (observed zero)

    if (out.record_size != kRecordSize) {
        throw std::runtime_error(
            "LytLayout: unexpected record_size (got " +
            std::to_string(out.record_size) + ", expected 1137)");
    }

    const std::size_t body_size = bytes.size() - kFileHeaderSize;
    if (body_size % kRecordSize != 0) {
        throw std::runtime_error(
            "LytLayout: file body is not a whole number of records");
    }
    const std::size_t n_records = body_size / kRecordSize;

    out.widgets.reserve(n_records);
    for (std::size_t i = 0; i < n_records; ++i) {
        std::size_t rec_off = kFileHeaderSize + i * kRecordSize;
        std::span<const std::uint8_t> rec = bytes.subspan(rec_off, kRecordSize);

        LytWidget w;
        w.name     = read_fixed_string(rec, kOffName, kSizeName);
        w.type     = read_u32_at(rec, kOffType);
        w.x        = read_i32_at(rec, kOffX);
        w.y        = read_i32_at(rec, kOffY);
        w.width    = read_i32_at(rec, kOffWidth);
        w.height   = read_i32_at(rec, kOffHeight);
        w.group_id = read_u32_at(rec, kOffGroupId);

        // Style slots: 9 × 33-byte slots starting at 0x1CC, each
        // holding either an ASCII "0xRRGGBBAA" colour literal or, for
        // atlas-driven widgets (radio/check/dropdown/list/scroll/...),
        // a "widgets\foo" art-path identifier.  We surface both views:
        // the raw NUL-terminated string and the parsed colour when the
        // text matches the literal shape.
        for (std::size_t k = 0; k < kColorSlotCount; ++k) {
            std::size_t off = kOffColor0 + k * kColorSlotStride;
            // Read up to the full 33-byte slot to capture the longer
            // art-path strings (e.g. "widgets\radio_n_g").
            std::string text = read_fixed_string(rec, off, kColorSlotStride);
            w.style_slots[k]       = text;
            w.style_slot_colour[k] = parse_lyt_color_literal(text);
        }


        // Sound slots: 32-byte names inside their 37-byte slots.
        w.sounds[0] = read_fixed_string(rec, kOffSound1, 32);
        w.sounds[1] = read_fixed_string(rec, kOffSound2, 32);
        w.sound_volume_pct[0] = read_u32_at(rec, kOffSound1Volume);
        w.sound_volume_pct[1] = read_u32_at(rec, kOffSound2Volume);

        // Image flags + binary fill colours.
        w.image_animated = rec[kOffImageAnimated] != 0;
        w.image_stretch  = rec[kOffImageStretch] != 0;
        w.fill_argb      = read_u32_at(rec, kOffFillArgb);
        w.fill_alt_argb  = read_u32_at(rec, kOffFillAltArgb);

        // Font slots: 16-byte names inside their 17-byte slots.
        w.fonts[0] = read_fixed_string(rec, kOffFont1, 16);
        w.fonts[1] = read_fixed_string(rec, kOffFont2, 16);

        if (w.type == 19) {
            // FUN_00607580 walks 5-byte records from LYT +0x32f and calls
            // FUN_00607e60(width, anchor_x, alignment), stopping at the -1
            // width sentinel or after 0x33 entries.
            for (std::size_t column = 0; column < kTableColumnMax; ++column) {
                const std::size_t off =
                    kOffTexturePath + column * kTableColumnStride;
                const std::int16_t width = read_i16_at(rec, off);
                if (width == -1) break;
                LytTableColumn decoded;
                decoded.width = width;
                decoded.alignment = rec[off + 2];
                decoded.anchor_x = read_i16_at(rec, off + 3);
                w.table_columns.push_back(decoded);
            }
        } else {
            w.texture_path = read_fixed_string(rec, kOffTexturePath, 33);
        }
        w.widget_art_base = read_fixed_string(rec, kOffWidgetArt,   33);

        // Animation block.  Reads as zero when the widget isn't
        // animated, so always parsing is safe.
        w.anim_frame_count    = read_u32_at(rec, kOffAnimFrameCount);
        w.anim_frame_duration = read_f32_at(rec, kOffAnimFrameDuration);
        for (std::size_t f = 0; f < 2; ++f) {
            std::size_t base = (f == 0) ? kOffAnimFrame0 : kOffAnimFrame1;
            for (int j = 0; j < 4; ++j) {
                w.anim_frame_rects[f][j] =
                    read_i32_at(rec, base + static_cast<std::size_t>(j) * 4);
            }
        }

        // PapyRes resource IDs.
        w.caption_id = read_u32_at(rec, kOffCaptionId);
        w.tooltip_id = read_u32_at(rec, kOffTooltipId);

        // Trailing parameter block (alignment + slider/spinner range).
        w.alignment     = read_u32_at(rec, kOffAlignment);
        w.param_min     = read_u32_at(rec, kOffParamMin);
        w.param_max     = read_u32_at(rec, kOffParamMax);
        w.param_default = read_u32_at(rec, kOffParamDefault);
        w.param_axis    = read_u32_at(rec, kOffParamAxis);
        w.param_aux     = read_u32_at(rec, kOffParamAux);
        w.item_value    = read_u32_at(rec, kOffItemValue);

        // Extra ASCII slots (button/radio constructors).
        w.extra_string_1 = read_fixed_string(rec, kOffExtraStr1, kSizeExtraStr1);
        w.extra_string_2 = read_fixed_string(rec, kOffExtraStr2, kSizeExtraStr2);

        // Widget-tree linkage + anchor system (all i16s).
        w.parent_group_id = read_i16_at(rec, kOffParentGroupId);
        w.own_group_id    = read_i16_at(rec, kOffOwnGroupId);
        w.parent_index    = read_i16_at(rec, kOffParentIndex);
        w.anchor_code     = static_cast<LytAnchor>(read_i16_at(rec, kOffAnchorCode));

        if (keep_raw) {
            w.raw.assign(rec.begin(), rec.end());
        }

        out.widgets.push_back(std::move(w));
    }

    return out;
}

}  // namespace opennr
