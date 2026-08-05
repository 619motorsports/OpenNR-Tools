#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opennr {

// Anchor corner code at on-disk offset 0x463.  The runtime uses this
// to position widgets relative to their parent's rect when the
// window is not at the 800x600 design resolution.
//
// Values confirmed against NR2003.exe's anchor resolver
// (FUN_0060d4d0): the only valid disk values across the 4631 shipped
// records are {-1, 0, 100, 101, 102, 103}.  -1 means "no parent /
// use viewport"; 0 means the widget opts out of anchor resolution.
enum class LytAnchor : std::int16_t {
    None        = -1,  // no parent (use viewport directly)
    Zero        =  0,  // disabled (still meaningful: 51 records)
    TopLeft     = 100,
    TopRight    = 101,
    BottomLeft  = 102,
    BottomRight = 103,
};

// Type-19 repurposes the nominal 0x32F string region as up to 51 packed
// five-byte column descriptors. The original constructor passes these fields
// to its inner table control as (width, anchor_x, alignment).
struct LytTableColumn {
    std::int16_t  width      = 0;
    std::int16_t  anchor_x   = 0;
    std::uint8_t  alignment  = 0;  // 0=left, 1=right, 2=center
};

// Parsed record from a Papyrus .lyt UI layout file.
//
// The on-disk record is a fixed 1137-byte tagged-union; this struct
// pulls out the universally-meaningful named fields and keeps the raw
// 1137 bytes around (`raw`) so downstream code can probe the
// type-specific regions whose semantics are still being pinned down
// (the trailing flag bytes 0x430..0x46F, the rare 0x118-0x12F u32s).
//
// See docs/formats/lyt_layout.md for the on-disk layout and the
// widget type-code table.
struct LytWidget {
    // Top-level identity / geometry.
    std::string                       name;            // up to 32 chars
    std::uint32_t                     type        = 0; // see kLytWidgetType*
    std::int32_t                      x           = 0;
    std::int32_t                      y           = 0;
    std::int32_t                      width       = 0;
    std::int32_t                      height      = 0;
    std::uint32_t                     group_id    = 0;

    // 9 polymorphic 33-byte string slots starting at on-disk offset
    // 0x1cc.  Each slot holds one of:
    //   - an empty (zero-padded) slot,
    //   - a 10-character "0xRRGGBBAA" colour literal, or
    //   - a longer ASCII path/identifier (e.g. "widgets\\radio_n" for
    //     atlas-driven widgets like radio buttons / dropdowns).
    // `style_slots[k]` is the raw NUL-terminated string from each slot
    // (empty when the slot is zero-padded) and `style_slot_colour(k)`
    // is the parsed colour if the slot's text matches the literal
    // shape, std::nullopt otherwise.
    std::array<std::string, 9>                    style_slots;
    std::array<std::optional<std::uint32_t>, 9>   style_slot_colour;

    // Up to 2 short-clip .wav paths (mouse-down "*_t.wav" and
    // mouse-release "*_s.wav" by convention).  Empty string for unused
    // slots.
    std::array<std::string, 2>        sounds;
    // Per-clip volume percent (0x1a3 / 0x1c8); the engine's base
    // widget ctor (FUN_00609880) scales it by 0.01 into a gain.
    // Shipped data uses 1 on every sound-bearing widget.
    std::array<std::uint32_t, 2>      sound_volume_pct{};

    // Image flags (u8s at 0x180 / 0x181, type-9/25 ctor FUN_00613f50):
    // `image_animated` arms the strip-animation driver after create;
    // `image_stretch` selects the stretched/tiled sprite path that
    // consumes (width, height) — without it images draw at natural
    // texture size regardless of width/height.
    bool                              image_animated = false;
    bool                              image_stretch  = false;

    // Binary 0xAARRGGBB colours at 0x2f9 / 0x2f5 (after the 9 string
    // slots).  `fill_argb` is the type-6 rect fill (FUN_00615640) and
    // the type-16 listbox secondary colour; `fill_alt_argb` is the
    // listbox primary (0xff1d1d1d row background in shipped data).
    // Every shipped rect keeps its colour ONLY here — the string
    // slots are empty on all 92 coloured rects.
    std::uint32_t                     fill_argb     = 0;   // 0x2F9
    std::uint32_t                     fill_alt_argb = 0;   // 0x2F5

    // Up to 2 font names; primary at 0x305, secondary at 0x316.
    std::array<std::string, 2>        fonts;

    // Resource path: a texture/resource name for image and resource-backed
    // widgets. Type-19 stores binary column descriptors here and therefore
    // leaves this string view empty. Type-4 constructors do not consume this
    // slot as placeholder text.
    std::string                       texture_path;

    // Type-19 packed descriptors at 0x32F: int16 width, u8 alignment,
    // int16 anchor_x, repeated until width == -1 (at most 51 entries).
    std::vector<LytTableColumn>       table_columns;

    // Type-2 (radio/check/tab) widget art base name (e.g.
    // "widgets\\radio_s_g").  Empty otherwise.
    std::string                       widget_art_base;

    // Animation block (only set for multi-frame type-9 image widgets;
    // zero-initialised otherwise).
    std::uint32_t                     anim_frame_count    = 0;
    float                             anim_frame_duration = 0.0f;
    // Two position records are decoded as (start_x, start_y, end_x, end_y).
    // The following on-disk block is retained in raw layout bytes; it is not
    // a third position record (credits.lyt repeats the rate bits there).
    std::array<std::array<std::int32_t, 4>, 2> anim_frame_rects{};

    // Caption + tooltip resource IDs into the Win32 RT_STRING table in
    // NR2003.exe (offsets 0x327, 0x32B).  Read as u32s by the runtime;
    // both forwarded to almost every interactive widget constructor
    // (button, radio, listbox, ...).  Zero when no resource is
    // referenced.  Resolved via `opennr::PeStringTable` (see
    // `src/fs/pe_string_table.h`).  Note: PapyRes.dll has only 3 small
    // RT_STRING blocks (76, 80, 751) — those are unrelated UI defaults,
    // NOT the menu captions.
    std::uint32_t                     caption_id = 0;
    std::uint32_t                     tooltip_id = 0;

    // Text alignment at 0x431, u32.  Verified 2026-07-11 against the
    // TextSprite autofit/position routine (FUN_00615520):
    //   0 = left   (x unchanged),
    //   1 = right  (x becomes the RIGHT edge: x - width + 1),
    //   2 = centre (x - width/2).
    std::uint32_t                     alignment = 0;

    // Slider / spinner / text-input parameter block (0x435..0x443).
    // For sliders (type 12) and spinners (type 13) these are the
    // value range; for text inputs (type 4), param_default is the
    // authored max-character count. All four are u32s
    // in the on-disk record, even though they store small integers
    // in shipped data.
    std::uint32_t                     param_min     = 0;   // 0x435
    std::uint32_t                     param_max     = 0;   // 0x439
    std::uint32_t                     param_default = 0;   // 0x43D (text max)
    std::uint32_t                     param_axis    = 0;   // 0x441 {0,1,2}
    // 0x445: type-19 tables feed it to a table setter (FUN_00608c80);
    // buttons treat nonzero as "use the pressed colour slot while
    // active/selected" (FUN_0060adf0).  Values 1/2/5 in shipped data.
    std::uint32_t                     param_aux     = 0;   // 0x445
    // 0x459: copied into every widget by the base ctor (→ +0x199).
    // Nonzero only on type-2 radio/tab items (1..3) in shipped data —
    // the item's value within its group.
    std::uint32_t                     item_value    = 0;   // 0x459

    // Two ASCII string slots forwarded to button / radio constructors
    // (offsets 0x449 8-byte slot, 0x451 28-byte slot).  Always empty
    // in shipped data but the loader supports them — kept here so
    // round-tripping through saved files preserves them.
    std::string                       extra_string_1;  // 0x449
    std::string                       extra_string_2;  // 0x451

    // Widget-tree linkage (i16s at 0x45D, 0x45F).  The runtime walks
    // sibling widgets as long as the next record's parent_group_id
    // (0x45D) matches the current record's own_group_id (0x45F).
    //
    //   parent_group_id == -1  →  top-level widget (no parent group)
    //   own_group_id    == 0   →  leaf widget (exposes no group)
    //   else                   →  group container; children are
    //                              consecutive following records
    //                              whose parent_group_id == this
    //                              own_group_id.
    std::int16_t                      parent_group_id = -1;
    std::int16_t                      own_group_id    =  0;

    // Anchor system (i16s at 0x461, 0x463).
    //   parent_index  = -1 → resolve against the viewport
    //                  else → flat index into the parent dialog's
    //                         widget array (so the parent rect is
    //                         widgets[parent_index]'s on-screen rect).
    //   anchor_code   = one of LytAnchor; selects which corner of
    //                   the parent rect (x, y) is offset from.
    std::int16_t                      parent_index = -1;
    LytAnchor                         anchor_code  = LytAnchor::None;

    // Verbatim 1137-byte record buffer.  Useful for clients that need
    // to inspect type-specific fields whose semantics aren't yet pinned
    // down (e.g. trailing alignment / shadow toggles at 0x431, 0x43d).
    // Empty when not requested.
    std::vector<std::uint8_t>         raw;
};

// Top-level file: header + N widget records.
struct LytLayout {
    std::uint32_t              version    = 0;   // observed always 8
    std::uint32_t              record_size = 0;  // observed always 1137
    std::vector<LytWidget>     widgets;

    // Parse the full file.  When `keep_raw=true` each LytWidget keeps
    // the verbatim 1137-byte record buffer (more memory but lets
    // tests inspect TBD fields).
    static LytLayout parse(std::span<const std::uint8_t> bytes,
                           bool keep_raw = false);
};

// Parse a literal of the form "0xRRGGBBAA" (10 chars, lowercase or
// upper).  Returns std::nullopt if the slot is empty (leading NUL) or
// malformed.  The 32-bit return is packed as R<<24 | G<<16 | B<<8 | A.
std::optional<std::uint32_t>
parse_lyt_color_literal(std::string_view ascii);

// Pick the primary font basename for a widget per NR2003's LYT-editor
// convention (decoded from the 4,631 shipped widget records):
//
//   - Type 1 (caption "AdvanceButton"): primary face is `fonts[1]` —
//     the LYT editor stores the caption font in the SECONDARY slot
//     for type 1 specifically.  Verified: 212/213 shipped records put
//     the face in slot 1 and leave slot 0 empty (1 outlier duplicates
//     it in both slots, with the same name).
//   - Type 0 (atlas / chevron buttons): no text, both slots empty.
//   - Types 2 / 4 / 5 / 8 / 16 / 19 / 26 (radio/check/tab, text input,
//     label, wrapped label, listbox, table, multi-line edit): primary
//     face is `fonts[0]`.  Types 2 / 4 / 26 often duplicate it into
//     `fonts[1]`, but the primary is always slot 0 across all 2,135
//     shipped records that have any font set.
//   - Types 6 / 9 / 11 / 12 / 13 / 14 / 15 / 17 / 18 / 22 (rect /
//     image / slider / spinner / scrollbar / dropdown / containers):
//     never have fonts; returns "".
//
// Returns "" when both slots are empty.  Free function so non-renderer
// callers (asset preloaders, tests) can use it without instantiating
// UiRenderer.
std::string lyt_font_basename_for_widget(const LytWidget& w);

}  // namespace opennr
