#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opennr {

// One corner of a NASCAR setup (LF, RF, LR, RR).
struct CornerSetup {
    float        tire_pressure   = 0.0f;  // PSI (cold, gauge)
    std::int32_t bump_low        = 0;     // shock low-speed bump
    std::int32_t bump_high       = 0;     // shock high-speed bump (zero in stock)
    std::int32_t rebound_low     = 0;     // shock low-speed rebound
    std::int32_t rebound_high    = 0;     // shock high-speed rebound
    float        spring_rate     = 0.0f;  // signed; magnitude in N/m
    // DGTS +0x60..0x6c. The setup UI label suggests spring rubbers, but the
    // shipped executable has no runtime-physics accessor for this quartet
    // and all 516 installed corner records are zero. Keep the serialized
    // value without applying an unverified physical meaning.
    float        spring_rubber   = 0.0f;
    // DGTS +0x70..0x7c. FUN_0055e9b0 passes this directly as
    // FUN_0055e490's cubic-transition width (0.031750001 in the stock corpus).
    float        spring_transition_width_m = 0.0f;
    float        camber          = 0.0f;  // radians
    // FUN_00424860 state IDs 0x24..0x27. LF/LR/RR are serialized at
    // DGTS +0x90/+0x94/+0x98; RF is reconstructed on the original
    // quarter-inch grid by FUN_00425a30.
    float        ride_height_target_m = 0.0f;
};

// Parsed view of the DGTS sub-chunk in a Papyrus .sim file. See
// docs/formats/pgts_container.md for the field-level mapping.
struct CarSetup {
    std::array<CornerSetup, 4> corner;  // LF, RF, LR, RR

    // DGTS +0xfc / setup slot 0x40. Cross-weight ("wedge"), stored in
    // kilograms; the garage / black-box "WEDGE:" control displays pounds
    // (lb = round(kg * 2.205)), range -150..150 lb step 5. Proven by the
    // FUN_00485e70 "WEDGE:" panel (label id 0x4c08 paired with slot 0x40)
    // and by FUN_00462380 emitting the "Wedge" pit message (id 0x3cd8)
    // when slot 0x40 changes — the same proof shape as grille tape.
    float        wedge              = 0.0f;
    // DGTS +0xf4 / setup slot 0x3e. Weight left/right bias, kilograms on
    // disk; the garage shows pounds (lb = round(kg * 2.205)), descriptor
    // range -50..150 lb step 5. Same "WEDGE:" black box: the "Left/Right
    // Bias:" label (id 0x4c87) pairs with the slot-0x3e value row.
    float        left_right_bias    = 0.0f;
    // DGTS +0xf8 / setup slot 0x3f. Weight front/rear bias, kilograms on
    // disk; shown in pounds, descriptor range -200..150 lb step 5. The
    // "Front/Rear Bias:" label (id 0x4c86) pairs with the slot-0x3f row.
    float        front_rear_bias    = 0.0f;
    // DGTS +0x100/+0x104 / setup slots 0x41/0x42. These are the editable
    // front/rear tire-radius deltas used to produce stagger. The garage
    // shows circumference difference in inches: stored_m * 39.37 * 6.28,
    // range 0..4 in, step 0.25. Descriptor 0x006eddd4 and button handler
    // FUN_00482a60 establish the exact slots and conversion callbacks.
    float        front_stagger_radius_delta_m = 0.0f;
    float        rear_stagger_radius_delta_m  = 0.0f;
    // DGTS +0xa0 / setup slot 0x29. Front toe-out, radians on disk. The
    // garage "Front Toe-Out:" control displays inches (in = rad * -27.9),
    // range -0.2..0.2 in step 0.025. Proven by FUN_00485780's
    // "Front Toe-Out:" label (id 0x4be2) paired with the slot-0x29 getter.
    float        front_toe_out      = 0.0f;
    // DGTS +0xa4 / setup slot 0x2a. Rear toe-out, radians on disk. The
    // transfer-map slot is authoritative; the front/rear (not LF/RF)
    // reading follows from the proven front toe-out at slot 0x29 and the
    // garage's N3ToeRear adjuster.
    float        rear_toe_out       = 0.0f;
    float        front_caster_lf    = 0.0f;  // radians
    float        front_caster_rf    = 0.0f;  // radians
    // DGTS +0xb0/+0xb4. FUN_00424860 and FUN_005601c0 consume their average
    // and difference as the left/right rear track-bar geometry, in metres.
    float        rear_track_bar_lr  = 0.0f;
    float        rear_track_bar_rr  = 0.0f;
    // DGTS +0xf0 / setup slot 0x3d. Disk stores (display degrees - 45) as
    // f32; the original callback restores the 45..70 degree garage value.
    float        rear_spoiler_angle_deg = 0.0f;
    float        grille_tape        = 0.0f;
    // DGTS +0xa8/+0xac integer selections. FUN_004257a0 converts them through
    // separate front/rear anti-roll stiffness equations.
    std::int32_t front_bar_setting  = 0;
    std::int32_t rear_bar_setting   = 0;
    // DGTS +0xb8 / setup slot 0x2f. The 0..1 descriptor is confirmed, but
    // no executable consumer or resource label has established its meaning.
    std::int32_t unknown_0b8        = 0;
    // DGTS +0xbc / setup slot 0x30. Steering ratio, stored as a float
    // (e.g. 30.0) and displayed "N:1"; schema range 12..32 step 1
    // default 18. Proven by the "STEERING RATIO:" panel (id 0x4bcd)
    // reading slot 0x30 through the setup table and the "%2ld:1" format.
    // The earlier "fuel cell gallons" label was a range guess (12..32 also
    // fits gallons) and is disproven: values are 18 at Bristol vs 32 at
    // Daytona/Talladega — steering speed, not a fuel tank (Daytona uses a
    // smaller 13.5-gal cell).
    float        steer_ratio        = 0.0f;
    // DGTS +0xc0 / setup slot 0x31. Fuel load is stored as kilograms. The
    // garage descriptor exposes whole US gallons through callbacks:
    // display = round(kg * 0.3524 + 0.1), stored = gallons / 0.3524.
    // Its schema is 1..22 gallons, step 1, default 1.
    float        fuel_load_kg       = 0.0f;
    float        brake_bias         = 0.0f;
    // DGTS +0xec. This is bit-identical to +0xe8 in every shipped .sim/.acd
    // file, so it is not evidence for a separate steering-ratio field.
    float        unknown_0ec        = 0.0f;

    // ---- Drivetrain: gear + final-drive INDICES ---------------------------
    // The garage Drivetrain panel stores each gear and the final drive as an
    // integer index into the shared teeth-ratio table (see gear_ratio_table.h,
    // NR2003 FUN_00425690).  Gear count (slot 0x33) is 4 for every NASCAR
    // chassis; reverse (slot 0x34) is always 0.  gear_index[0..3] are gears
    // 1..4 at DGTS +0xd0/+0xd4/+0xd8/+0xdc (slots 0x35..0x38); the final drive
    // is +0xe0 (slot 0x39).  Gears 5/6 (slots 0x45/0x46) lie past the 272-byte
    // v13 body, so they are NOT persisted — the live setup object keeps them at
    // the schema defaults 66/67, which parse_dgts reproduces.
    std::int32_t gear_count        = 0;     // DGTS +0xc8 / slot 0x33
    std::int32_t reverse_index     = 0;     // DGTS +0xcc / slot 0x34
    std::array<std::int32_t, 6> gear_index = {};  // gears 1..6
    std::int32_t final_drive_index = 0;     // DGTS +0xe0 / slot 0x39

    // Raw bytes of the DGTS body for fields we haven't fully decoded.
    // Callers can re-interpret as new fields are mapped.  SimFile::write
    // starts from a copy of this and patches every typed field over the
    // top, so the still-unmapped bytes survive a save byte-exact.
    std::vector<std::uint8_t> raw_dgts;
};

// Exact float-callback boundaries used by setup slots 0x41/0x42. The
// original uses 6.28 (not a higher-precision 2*pi) and 39.37/0.0254.
float stagger_inches_from_radius_delta(float radius_delta_m) noexcept;
float stagger_radius_delta_from_inches(float stagger_inches) noexcept;

// Recompute the RF ride-height target (corner[1]) from LF/LR/RR, mirroring
// NR2003's FUN_00425a30: RF is not stored on disk — it is derived so the
// front rake matches the rear, quantized to the quarter-inch grid.  The garage
// calls this after any LF/LR/RR ride-height edit (matching the engine, which
// runs the reconstruction after each adjust) so the derived RF stays correct.
void reconstruct_rf_ride_height(CarSetup& setup);

// One Papyrus .sim or .acd file decoded.
struct SimFile {
    std::uint32_t pgts_version = 0;       // outer chunk version (5 in stock)
    std::uint32_t hgts_version = 0;       // inner header version (3 in stock)
    std::uint32_t dgts_version = 0;       // setup version (0xD in stock)
    std::string   embedded_name;          // e.g. "qualify.cup.sim"
    std::string   description;            // TGTS body text (trimmed of NULs)

    CarSetup      setup;                  // parsed DGTS values

    // Full HGTS / TGTS sub-chunk bodies, captured verbatim at parse time.
    // write() reuses them so a save round-trips byte-exact without having
    // to fully decode the record header or re-lay-out the description slot.
    std::vector<std::uint8_t> raw_hgts;
    std::vector<std::uint8_t> raw_tgts;

    static SimFile parse(std::span<const std::uint8_t> bytes);

    // Serialize back to a PGTS container.  The DGTS body is rebuilt from
    // `setup`'s typed fields over a copy of `setup.raw_dgts` (so both edited
    // fields and still-unmapped bytes are correct); HGTS/TGTS are reused
    // verbatim when captured.  When `new_embedded_name` is non-empty the two
    // HGTS name copies are patched to it.  Loading a shipped file and writing
    // it back with no edits reproduces the original bytes.
    std::vector<std::uint8_t> write(std::string_view new_embedded_name = {}) const;
};

}  // namespace opennr
