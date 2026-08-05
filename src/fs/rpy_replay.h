#pragma once

#include "math/vec.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace opennr {

// Decoded structure of a Papyrus .rpy ("RPLY") replay file.
//
// See docs/formats/rpy_replay.md for the on-disk format. The container
// is a tree of byte-reversed FourCC chunks (the same convention as
// .stp / .mip / .sim / .cam):
//
//   RPLY                                       outer envelope (v=5)
//   ├── RPHD                                   replay header
//   ├── WKNF                                   weekend / session info
//   ├── DRLS                                   driver list (container)
//   │   └── DRNT * N                           per-driver entries
//   ├── LPTB * 4                               lap tables (one per stage)
//   │   └── LPRO * M                           per-car lap-time records
//   ├── RPTP                                   structural frame/event body
//   └── RPRS                                   trailer/sentinel (size 0)
//
// The RPTP body contains exactly RPHD.frame_count length-delimited frame
// blocks. Each frame contains aligned, typed events. The parser retains the
// bytes and builds compact structural indices; event payloads are decoded
// lazily or left opaque.

// One driver list entry. NR2003 always emits at least the player slot
// ("The Player") and the pace-car slot ("Pace Car") plus one row per
// AI driver — so |drivers| typically equals 2 + (race entry count).
struct RpyDriverEntry {
    std::string  first_name;     // 16-byte ASCII slot, NUL-trimmed
    std::string  last_name;      // 16-byte ASCII slot, NUL-trimmed
    std::string  car_file;       // 32-byte ASCII path, NUL-trimmed
                                  //   e.g. "24_j_gordon.cup.car"
    std::uint32_t mfg_index   = 0;     // body+0x90
    std::uint32_t flags_a     = 0;     // body+0x94
    std::uint32_t reserved_88 = 0;     // body+0x98 (always 0 in samples)
    std::uint32_t car_number  = 0;     // body+0x9c (e.g. 0x18 for #24)
    std::uint32_t skill_or_xp = 0;     // body+0xa0 (e.g. 0xfa = 250)
    std::uint32_t list_id     = 0;     // body+0xa4 (incrementing per driver)
    std::uint32_t race_pos    = 0;     // body+0xa8 (1-based grid pos)
    std::array<std::uint8_t, 3> tail{};  // body+0xac..+0xaf trailing flags
    // body+0xac — `0` for the human player slot, `1` for both pace car
    // and AI drivers.  Mirrors the engine's is-AI-controlled split.
    bool ai_controlled = false;
    // body+0xad — `1` for the player slot and every AI racer; `0` only
    // for the pace car slot.  Captures "this driver is competing for
    // race position" — pace car is on track but doesn't race.
    bool is_racer = false;
    // body+0xae — observed `0x03` only for the player slot; `0x00` for
    // everyone else.  Likely a "primary controller / highlight in
    // standings" UI flag; surfaced as raw byte while semantics are
    // partially decoded.
    std::uint8_t player_flags = 0;
    std::vector<std::uint8_t> raw;       // full 175-byte body for unmapped fields
};

// One lap-time entry (LPRO sub-record). The body is 76 bytes:
//   +0x00 u32  car_index            (matches DRLS slot index)
//   +0x04 u32  field_at_4           (often a packed flag word)
//   +0x08 u32[18] cumulative lap timestamps in milliseconds, with
//                 one slot per completed lap. Slots that haven't been
//                 reached yet hold a sentinel.
struct RpyLapRow {
    std::uint32_t car_index = 0;
    // Packed status word.  Bit 30 (0x40000000) is a per-car status
    // flag (observed set on alternating-rank front-pack cars in the
    // bundled test.rpy, cleared on the back of the grid); the low 30
    // bits hold a per-car time-like value (~395k..398k ms in
    // test.rpy, monotonic with grid position, spread ~3.7s across
    // the field — consistent with a per-car race-anchor / pace-line
    // crossing time).  Partial decode; semantics still under
    // investigation.  Use `field_at_4_low()` and `field_at_4_bit30()`
    // for the split.
    std::uint32_t field_at_4 = 0;
    bool          field_at_4_bit30() const {
        return (field_at_4 & 0x40000000u) != 0;
    }
    std::uint32_t field_at_4_low() const {
        return field_at_4 & 0x3FFFFFFFu;
    }
    std::array<std::uint32_t, 18> lap_times_ms{};
    std::vector<std::uint8_t> raw;
};

// One LPTB stage. Stock files emit four LPTBs in a row; the first three
// are usually empty (size=0) and the fourth carries the actual race.
// We store all four so the index is preserved.
struct RpyLapTable {
    std::vector<RpyLapRow> rows;
};

// Parsed RPHD header.
struct RpyHeader {
    std::uint32_t frame_count    = 0;   // body +0x00
    std::uint32_t rptp_body_size = 0;   // body +0x04 (must match RPTP size)
    std::uint32_t event_count    = 0;   // body +0x08 (purpose still tentative)
    std::uint32_t reserved_0c    = 0;   // body +0x0c
    std::string   player_first;          // body +0x10 (8 bytes)
    std::string   player_last;           // body +0x20 (8 bytes)
    std::uint32_t reserved_38    = 0;   // body +0x38
    // Optional UTF-16LE lesson/replay description beginning at +0x3c.
    // Stock ordinary race replays leave it empty; Driving Lessons use it
    // for the right-hand summary pane.
    std::string   summary;
    std::vector<std::uint8_t> raw;       // full RPHD body
};

// One row in the 4-entry session table embedded in the WKNF tail.
// NR2003 ships four sessions per race weekend: Practice, Qualifying,
// Happy Hour (warmup), and Race; the table appears at WKNF body
// offset 0x6c in this fixed order.  Each record is 8 bytes with the
// duration float misaligned at byte offset 3:
//   +0x00 u8  flags_lo
//   +0x01 i16 lap_limit  (0x7fff = "no lap limit")
//   +0x03 f32 duration_seconds
//   +0x07 u8  type_tag   (5=practice, 4=qualify, 7=happy_hour, 0=race
//                          in the bundled test.rpy)
//
// These fields describe the configured weekend rows and are used by the
// stock row-enable/advance logic.  The recovered replay checkpoint path
// addresses a separate runtime replay table, so this parser does not claim
// that either field alone drives the clean `SessionPhase` machine.  For
// Practice and Happy Hour the lap_limit ships as i16-max (no limit); for
// Qualifying shipped captures use one or two laps depending on the event
// (the stock race-weekend captures commonly use 2); for Race it carries the
// absolute lap count (raceLength % × track
// default_event_laps rounded — 17 laps for Atlanta at raceLength=5).
struct RpySessionEntry {
    std::uint8_t  flags_lo        = 0;
    std::int16_t  lap_limit       = 0;
    float         duration_seconds = 0.0f;
    std::uint8_t  type_tag        = 0;

    bool enabled_for_auto_advance() const noexcept {
        const bool has_cap = duration_seconds != 0.0f || lap_limit != 0;
        const bool empty_practice = duration_seconds == 0.0f &&
                                    lap_limit == 0x7fff;
        const bool infinite_sentinel = duration_seconds == 604800.0f &&
                                       lap_limit == 0;
        return has_cap && !empty_practice && !infinite_sentinel;
    }
};

// Parsed WKNF header (track + session metadata).  The 173-byte body
// contains track/class names, weather state, a session_id (also
// stamped at the start of RPTP), AI field size, race-length percent,
// and a 4-entry session table.  A handful of fields in the tail are
// still un-decoded — see `raw` for the full body.
struct RpyWeekend {
    std::string  track_name;            // body +0x00 (32-byte slot)
    std::string  class_name;            // body +0x20 (32-byte slot, e.g. "cup")

    // body +0x40 — 4-byte session id.  Also stamped at the start of
    // the RPTP body (offset +0x14 inside the first frame block),
    // so a tool that recovers a truncated RPTP from a fresh
    // weekend / replay pair can match them up.
    std::uint32_t session_id      = 0;

    // body +0x5c — temperature in °F, low/high byte of a u16
    // (constant-weather replays stash the same value in both).
    std::uint8_t  temperature_f   = 0;

    // body +0x60 — raceLength as a 1..100 percent of the track's
    // default_event_laps.  Mirrors the `[Single Race] raceLength`
    // value the player picked on trksel.
    std::uint32_t race_length_pct = 0;

    // body +0x66 — `NumAI` (computer opponents) the weekend was
    // configured with.  Player slot + pace car slot are separate
    // and not included in this count.
    std::uint8_t  num_ai          = 0;

    // body +0x6c..+0x84 — the 4 session entries in fixed order:
    //   [0] Practice, [1] Qualifying, [2] Happy Hour, [3] Race.
    std::array<RpySessionEntry, 4> sessions{};

    std::vector<std::uint8_t> raw;      // full 173-byte body — the
                                         // timestamp at body+0x44..+0x50
                                         // and a handful of trailing
                                         // bytes are still under
                                         // investigation.
};

// Compatibility view of the first 28 RPTP bytes. These bytes are the first
// frame header followed by its initial type-4 event, not a stream preamble.
struct RpyRptpHeader {
    std::uint32_t session_id = 0;        // initial type-4 record's session_id (matches WKNF.session_id)
    std::array<std::uint8_t, 28> raw{};  // first 28 bytes for un-decoded fields
};

// One decoded type-4 session checkpoint record (20 bytes total).
// The first occurrence is in the first frame at body offset 8
// (session-init, sim_time = 0); the remaining ~164 fire
// periodically (~once every 5 simulated seconds in test.rpy),
// stamping the current sim_time alongside the session_id. The stock
// consumer decodes the three state bytes through the accessors below.
struct RpyRptpType4 {
    std::uint32_t body_offset    = 0;   // RPTP-body-relative record offset
    std::uint8_t  sub_tag        = 0;   // payload byte 0 — always 0x09 in test.rpy
    std::array<std::uint8_t, 3> state_bytes{};  // payload bytes 1..4
    float         sim_time       = 0.0f; // payload bytes 4..8 — seconds since session start (resets / jumps at phase transitions)
    std::uint32_t session_id     = 0;   // payload bytes 8..12 — matches RpyWeekend.session_id
    std::uint32_t reserved       = 0;   // payload bytes 12..16 — always 0 in test.rpy
    // Stock FUN_005accd0 decodes the packed state word as a signed 8-bit
    // index into its local/background session table, not SessionPhase.
    std::int32_t replay_session_index() const noexcept {
        const auto packed = static_cast<std::uint16_t>(state_bytes[1]) |
                            (static_cast<std::uint16_t>(state_bytes[2]) << 8);
        const auto shifted = static_cast<std::int16_t>(packed << 4);
        return static_cast<std::int32_t>(shifted) >> 8;
    }
    std::uint8_t replay_state_byte() const noexcept { return state_bytes[0]; }
    std::uint8_t replay_state_low_nibble() const noexcept {
        return static_cast<std::uint8_t>(state_bytes[2] & 0x0f);
    }
    std::uint8_t replay_state_high_nibble() const noexcept {
        return static_cast<std::uint8_t>(state_bytes[2] >> 4);
    }
};

// Exact packed view of the 16-byte payload carried by RPTP event type 11.
// FUN_005855a0 writes this record for each live 3-D debris object; playback
// FUN_00584e40 keys a 32-slot object pool by slot+generation and recreates
// either an anonymous chunk (kind 1) or detached body panel (kind 2).
struct RpyRptpType11DebrisState {
    std::uint32_t body_offset = 0;
    std::uint32_t frame_block = 0;
    std::uint8_t pool_slot = 0;          // dword0 bits 0..4
    std::uint32_t generation_id = 0;     // dword0 bits 5..22 (18 bits)
    std::uint16_t height_code = 0;       // dword0 bits 23..31, atan quantized
    std::uint8_t kind = 0;               // dword1 bits 0..1: 1 chunk, 2 panel
    std::int32_t lateral_code = 0;       // dword1 bits 2..18, signed 17-bit atan code
    std::uint32_t along_track_code = 0;  // dword2 bits 0..23, unsigned
    std::array<std::uint8_t, 3> orientation_codes{}; // bytes 11..13
    std::uint16_t appearance = 0;        // bytes 14..15: car/panel/paint bits

    float along_track() const noexcept {
        return static_cast<float>(along_track_code) * 0.0005f;
    }
    float lateral() const noexcept;
    float height_above_ground() const noexcept;
    // Raw byte order used by FUN_005f5a90: Z, then Y, then X Euler angles.
    std::array<float, 3> orientation_rad() const noexcept;
    // The clean debris renderer consumes X/Y/Z and composes Rz*Ry*Rx.
    Vec3 renderer_orientation_xyz() const noexcept;
    std::uint8_t car_slot() const noexcept {
        return static_cast<std::uint8_t>(appearance & 0x7fu);
    }
    std::uint8_t panel_id() const noexcept {
        return static_cast<std::uint8_t>((appearance >> 7) & 0x0fu);
    }
    std::uint8_t paint_id() const noexcept {
        return static_cast<std::uint8_t>((appearance >> 11) & 0x07u);
    }
    std::uint8_t appearance_reserved() const noexcept {
        return static_cast<std::uint8_t>((appearance >> 14) & 0x03u);
    }
};

struct RpyRptpType11BlockIndex {
    std::uint32_t first_state = 0;
    std::uint16_t state_count = 0;
};

struct RpyMarkerExtensionPoint {
    std::int16_t x_offset_code = 0; // signed 10-bit, 1/128 m
    std::int16_t y_offset_code = 0; // signed 10-bit, 1/128 m
    std::int8_t  z_offset_code = 0; // signed 8-bit, 1/128 m
    std::uint8_t intensity_nibble = 0; // high nibble of vertex intensity byte
    Vec3 world_position{};
};

// One tire-mark quad appended to a type-14 frame marker
// (35 bytes on the wire).  Decoded 2026-07-11 from the single mid-race
// burst in test.rpy. World position is signed 1/128-metre fixed point;
// the 12 packed bytes are three auxiliary vertices whose signed XYZ offsets
// are added to that base. `object_id` is car_slot*4+wheel_index, and the
// marker's frame index repeats at the tail. Raw bytes remain preserved.
struct RpyMarkerExtensionEntry {
    std::int32_t  x_fixed   = 0;    // world x, fixed-point (1/128 m)
    std::int32_t  y_fixed   = 0;    // world y
    std::int32_t  z_fixed   = 0;    // world z
    std::uint16_t flags     = 0;    // low nibble: old-edge intensity
    std::array<std::uint8_t, 12> packed{}; // three auxiliary vertices
    std::uint8_t  object_id = 0;    // car_slot * 4 + wheel_index
    std::uint32_t reserved  = 0;    // 0 in test.rpy
    std::uint32_t frame_index = 0;  // repeats the marker frame index

    Vec3 base_position() const noexcept;
    std::array<RpyMarkerExtensionPoint, 3>
    auxiliary_vertices() const noexcept;
    std::uint8_t start_intensity_nibble() const noexcept {
        return static_cast<std::uint8_t>(flags & 0x0fu);
    }
    std::uint8_t car_slot() const noexcept { return object_id / 4u; }
    std::uint8_t wheel_index() const noexcept { return object_id % 4u; }
};

// Runtime input consumed by the original FUN_005b4e60 encoder: four ribbon
// vertices and one intensity byte per vertex. Only each byte's high nibble
// survives on disk.
struct RpyTireMarkQuadState {
    std::array<Vec3, 4> vertices{};
    std::array<std::uint8_t, 4> vertex_intensity_bytes{};
    std::uint8_t object_id = 0;
};

// Exact valid-domain type-14 entry encoder. Returns nullopt when a point is
// non-finite/out of i32 range or an auxiliary delta exceeds signed 10/10/8
// bits, matching FUN_005b4e60's rejection boundary.
std::optional<RpyMarkerExtensionEntry> encode_rpy_tire_mark_entry(
    const RpyTireMarkQuadState& state,
    std::uint32_t frame_index) noexcept;

// A type-14 marker whose logical size exceeds the plain 8-byte form:
// the extension carries `entries` tire-mark quads.
struct RpyMarkerExtension {
    std::uint32_t frame_index = 0;
    std::uint32_t frame_block = 0;
    std::vector<RpyMarkerExtensionEntry> entries;
};

// Summary event-type counts recovered from the structural RPTP index.
struct RpyRptpRecordCounts {
    std::uint32_t type9_kinematic   = 0;
    std::uint32_t type14_marker     = 0;
    std::uint32_t type14_extension_entries = 0;
    std::uint32_t type13_event      = 0;
    std::uint32_t type4_session_init = 0;
    std::uint32_t type3_large_state  = 0;
    // Per-state_code count for type-9 records.  The state_code is the
    // first u32 of the 12-byte type-9 payload (typical values: 1, 2, 5).
    // state_code=2 dominates (~once per frame); 1 and 5 appear paired
    // during the active race phase (see docs).
    std::uint32_t type9_state_code_2 = 0;
    std::uint32_t type9_state_code_1 = 0;
    std::uint32_t type9_state_code_5 = 0;
    std::uint32_t type9_state_code_other = 0;
};

// Exact packed fields of one 16-byte type-3 car-state sample. Spatial fields
// are absolute track-relative values, not deltas. Signed codes are already
// sign-extended. See docs/formats/rpy_replay.md for the bit allocation.
struct RpyCarSample {
    bool          status_bit = false;
    std::uint16_t tick = 0;             // modulo 1024
    std::uint8_t  throttle_code = 0;    // unsigned 5-bit tangent code
    std::int16_t  pitch_code = 0;       // signed 10-bit tangent code
    std::int8_t   steering_code = 0;    // signed 6-bit tangent code
    std::uint32_t along_track_code = 0; // unsigned 24-bit
    std::uint8_t  car_index = 0;        // 0..47
    bool          discontinuity = false;
    std::int16_t  roll_code = 0;        // signed 10-bit tangent code
    std::uint16_t rpm_code = 0;         // unsigned 10-bit tangent code
    bool          state_flag = false;
    std::uint16_t vertical_code = 0;    // unsigned 9-bit tangent code
    std::uint8_t  mode = 0;             // packed 2-bit mode
    std::int16_t  yaw_code = 0;         // signed 12-bit linear code
    std::int32_t  lateral_code = 0;     // signed 17-bit tangent code
    std::uint8_t  status_flags = 0;     // upper 3 bits
    std::array<std::uint8_t, 16> raw{};

    float along_track(float track_length = 0.0f) const noexcept;
    float lateral() const noexcept;
    float vertical() const noexcept;
    float yaw_rad() const noexcept;
    float pitch_rad() const noexcept;
    float roll_rad() const noexcept;
    float engine_rpm() const noexcept;
    float throttle() const noexcept;
    float steering_rad() const noexcept;
};

// One frame's worth of per-car snapshots, extracted as needed (lazy)
// from `frame_data`.  Mirrors the on-disk T3 frame-envelope.
struct RpyFrameSamples {
    std::uint32_t frame_index = 0;     // = T14 marker's frame_index
    std::vector<RpyCarSample> cars;    // sized by detected stride
};

// Compact indices into `frame_data`. Offsets are relative to the RPTP body.
// Event payload bytes remain in `frame_data`; no payload copies are made.
struct RpyRptpEventIndex {
    std::uint32_t body_offset = 0;
    std::uint16_t logical_size = 0;
    std::uint16_t type = 0;
};

struct RpyRptpFrameIndex {
    std::uint32_t body_offset = 0;
    std::uint32_t first_event = 0;
    std::uint16_t total_size = 0;
    std::uint16_t event_count = 0;
    std::uint16_t previous_frame_size = 0;
    std::uint16_t sequence = 0;
    std::uint16_t flags = 0;

    std::uint16_t playback_sequence() const noexcept { return sequence & 0x03ffu; }
};

struct RpyRptpMarkerIndex {
    std::uint32_t frame_index = 0;
    std::uint32_t frame_block = 0;
    std::uint32_t event_index = 0;
};

struct RpyReplay {
    std::uint32_t outer_version = 0;    // RPLY top-level version (5 in stock)

    RpyHeader     header;
    RpyWeekend    weekend;
    std::vector<RpyDriverEntry> drivers;        // from DRLS
    std::array<RpyLapTable, 4>  lap_tables;     // from the four LPTB chunks

    // The RPTP body is a packed binary record stream.  We surface the
    // raw bytes for advanced callers plus structural frame/event indices.
    std::vector<std::uint8_t> frame_data;        // copy of RPTP body bytes
    std::vector<RpyRptpFrameIndex> frame_blocks;
    std::vector<RpyRptpEventIndex> events;
    std::vector<RpyRptpMarkerIndex> markers;
    std::uint32_t rptp_bytes_consumed = 0;

    // 28-byte session header decoded from the front of `frame_data`.
    RpyRptpHeader rptp_header{};

    // All decoded type-4 session-checkpoint records.  The first
    // entry is the initial session-init event in the first frame at body
    // offset 8 (sim_time = 0); the rest fire periodically.
    std::vector<RpyRptpType4> rptp_type4_checkpoints;

    // Recorded detached panels and anonymous collision chunks (type 11).
    std::vector<RpyRptpType11DebrisState> rptp_type11_debris;
    // One range per physical RPTP frame block; preserves tape order across
    // replay-editor timeline cuts without a repeated full-vector scan.
    std::vector<RpyRptpType11BlockIndex> rptp_type11_blocks;

    // Type-14 markers that carry tire-mark quads — rare in test.rpy.
    // See RpyMarkerExtension.
    std::vector<RpyMarkerExtension> marker_extensions;

    // Compatibility summary of the structurally indexed type-14 events.
    std::uint32_t marker_count       = 0;
    std::uint32_t first_frame_index  = 0;
    std::uint32_t last_frame_index   = 0;

    // Structural event-type counts. See `RpyRptpRecordCounts`.
    RpyRptpRecordCounts rptp_counts{};

    // Parse a raw .rpy file. Throws on malformed envelopes.
    static RpyReplay parse(std::span<const std::uint8_t> bytes);

    // Lazy per-frame per-car decoder. Selects the greatest type-14 timeline
    // index not greater than `frame_index`, then decodes every following
    // type-3 event in that frame. Type-3 event size determines sample count.
    // Marker indices are not assumed monotonic because edited lesson replays
    // contain timeline cuts.
    //
    // `frame_index` is matched against the marker stream and need not
    // be exact: the closest marker whose index is <= frame_index is
    // used.
    RpyFrameSamples samples_at_frame(std::uint32_t frame_index) const;

    // Decodes all type-3 samples in physical tape-frame order. This is the
    // playback-facing API; unlike timeline marker values, block indices are
    // contiguous even across replay-editor cuts and markerless frames.
    RpyFrameSamples samples_at_block(std::uint32_t block_index) const;

    std::span<const RpyRptpType11DebrisState>
    debris_at_block(std::uint32_t block_index) const noexcept;
};

}  // namespace opennr
