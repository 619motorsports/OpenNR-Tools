#include "sim_file.h"

#include "core/byte_reader.h"
#include "fs/gear_ratio_table.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace opennr {

namespace {

bool match_tag(std::span<const std::uint8_t> buf, std::size_t pos, const char* tag) {
    if (pos + 4 > buf.size()) return false;
    return buf[pos]     == static_cast<std::uint8_t>(tag[0]) &&
           buf[pos + 1] == static_cast<std::uint8_t>(tag[1]) &&
           buf[pos + 2] == static_cast<std::uint8_t>(tag[2]) &&
           buf[pos + 3] == static_cast<std::uint8_t>(tag[3]);
}

std::size_t skip_padding(std::span<const std::uint8_t> buf, std::size_t pos, std::size_t end) {
    std::size_t orig = pos;
    while (pos < end && buf[pos] == 0x20 && pos - orig < 3) {
        if (pos + 4 <= end) {
            // If a real upper-case tag starts here, stop.
            std::uint8_t b0 = buf[pos];
            if (b0 != 0x20 && b0 >= 'A' && b0 <= 'Z') break;
        }
        ++pos;
    }
    return pos;
}

float read_f32(std::span<const std::uint8_t> buf, std::size_t pos) {
    std::uint32_t bits = static_cast<std::uint32_t>(buf[pos]) |
                          (static_cast<std::uint32_t>(buf[pos + 1]) << 8) |
                          (static_cast<std::uint32_t>(buf[pos + 2]) << 16) |
                          (static_cast<std::uint32_t>(buf[pos + 3]) << 24);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

std::int32_t read_i32(std::span<const std::uint8_t> buf, std::size_t pos) {
    std::uint32_t bits = static_cast<std::uint32_t>(buf[pos]) |
                          (static_cast<std::uint32_t>(buf[pos + 1]) << 8) |
                          (static_cast<std::uint32_t>(buf[pos + 2]) << 16) |
                          (static_cast<std::uint32_t>(buf[pos + 3]) << 24);
    return static_cast<std::int32_t>(bits);
}

std::uint32_t read_u32(std::span<const std::uint8_t> buf, std::size_t pos) {
    return static_cast<std::uint32_t>(buf[pos]) |
           (static_cast<std::uint32_t>(buf[pos + 1]) << 8) |
           (static_cast<std::uint32_t>(buf[pos + 2]) << 16) |
           (static_cast<std::uint32_t>(buf[pos + 3]) << 24);
}

CarSetup parse_dgts(std::span<const std::uint8_t> body) {
    CarSetup s;
    s.raw_dgts.assign(body.begin(), body.end());
    if (body.size() < 0x110) {
        // Don't fail; just leave fields at defaults.
        return s;
    }
    // Per-corner struct-of-arrays.
    for (int c = 0; c < 4; ++c) {
        s.corner[c].tire_pressure  = read_f32(body, 0x000 + c * 4);
        s.corner[c].bump_low       = read_i32(body, 0x010 + c * 4);
        s.corner[c].bump_high      = read_i32(body, 0x020 + c * 4);
        s.corner[c].rebound_low    = read_i32(body, 0x030 + c * 4);
        s.corner[c].rebound_high   = read_i32(body, 0x040 + c * 4);
        s.corner[c].spring_rate    = read_f32(body, 0x050 + c * 4);
        s.corner[c].spring_rubber  = read_f32(body, 0x060 + c * 4);
        s.corner[c].spring_transition_width_m =
            read_f32(body, 0x070 + c * 4);
        // The setup-varying wheel-relative camber angles are +0x80..0x8c.
        s.corner[c].camber         = read_f32(body, 0x080 + c * 4);
    }
    s.corner[0].ride_height_target_m = read_f32(body, 0x090); // state 0x24
    s.corner[2].ride_height_target_m = read_f32(body, 0x094); // state 0x26
    s.corner[3].ride_height_target_m = read_f32(body, 0x098); // state 0x27
    reconstruct_rf_ride_height(s);  // derive corner[1] (RF), not on disk
    // Front caster is the radian pair at +0x108/+0x10c.
    s.front_caster_lf    = read_f32(body, 0x108);
    s.front_caster_rf    = read_f32(body, 0x10c);
    // The disk values remain normalized fractions. The slot-0x28/0x3b schema
    // handlers expose them to the garage as integer percentages.
    s.brake_bias         = read_f32(body, 0x09c);
    // Front/rear toe-out (slots 0x29/0x2a). Radians on disk; the garage
    // shows inches (in = rad * -27.9).
    s.front_toe_out      = read_f32(body, 0x0a0);
    s.rear_toe_out       = read_f32(body, 0x0a4);
    s.front_bar_setting  = read_i32(body, 0x0a8);
    s.rear_bar_setting   = read_i32(body, 0x0ac);
    s.rear_track_bar_lr  = read_f32(body, 0x0b0);
    s.rear_track_bar_rr  = read_f32(body, 0x0b4);
    s.unknown_0b8        = read_i32(body, 0x0b8);
    // Steering ratio (slot 0x30). Float on disk (e.g. 30.0), shown "N:1".
    s.steer_ratio        = read_f32(body, 0x0bc);
    // Fuel load (slot 0x31), kilograms on disk; the garage displays gallons.
    s.fuel_load_kg       = read_f32(body, 0x0c0);
    // Weight biases (slots 0x3e/0x3f) and cross-weight / wedge (slot 0x40).
    // All kilograms on disk; shown in pounds (lb = kg * 2.205).
    s.left_right_bias    = read_f32(body, 0x0f4);
    s.front_rear_bias    = read_f32(body, 0x0f8);
    s.wedge              = read_f32(body, 0x0fc);
    s.front_stagger_radius_delta_m = read_f32(body, 0x100);
    s.rear_stagger_radius_delta_m  = read_f32(body, 0x104);
    s.grille_tape        = read_f32(body, 0x0e8);
    s.unknown_0ec        = read_f32(body, 0x0ec);
    // Slot 0x3d callback 0x004241f0: round(stored + 0.1) + 45.
    s.rear_spoiler_angle_deg = float(
        std::nearbyint(double(read_f32(body, 0x0f0) + 0.100000001f)) + 45.0);
    // Drivetrain gear + final-drive indices (integers).  Gears 1..4 and the
    // final drive live in the body; gears 5/6 (slots 0x45/0x46) are past its
    // end, so they hold the schema defaults 66/67 in the live setup object.
    s.gear_count        = read_i32(body, 0x0c8);
    s.reverse_index     = read_i32(body, 0x0cc);
    s.gear_index[0]     = read_i32(body, 0x0d0);
    s.gear_index[1]     = read_i32(body, 0x0d4);
    s.gear_index[2]     = read_i32(body, 0x0d8);
    s.gear_index[3]     = read_i32(body, 0x0dc);
    s.gear_index[4]     = kDefaultGear5Index;  // not on disk
    s.gear_index[5]     = kDefaultGear6Index;  // not on disk
    s.final_drive_index = read_i32(body, 0x0e0);
    return s;
}

}  // namespace

float stagger_inches_from_radius_delta(float radius_delta_m) noexcept {
    // Descriptor read callback 0x00423ec0:
    //   raw radius delta [m] * 39.37 [in/m] * 6.28 [circumference factor].
    return static_cast<float>(double(radius_delta_m) *
                              double(39.369998931884765625f) *
                              double(6.280000209808349609375f));
}

float stagger_radius_delta_from_inches(float stagger_inches) noexcept {
    // Descriptor write callback 0x00423ea0:
    //   displayed circumference delta [in] * 0.0254 [m/in] / 6.28.
    return static_cast<float>(double(stagger_inches) *
                              double(0.02539999969303607940625f) *
                              double(0.159235656261444091796875f));
}

void reconstruct_rf_ride_height(CarSetup& s) {
    // FUN_00425a30 preserves the LR->RR rake at the front and quantizes RF
    // to a quarter-inch.  The original adds 0.5 before x87 round-to-nearest.
    const float rear_delta = s.corner[3].ride_height_target_m -
                             s.corner[2].ride_height_target_m;
    const float rf_steps = float(
        (double(s.corner[0].ride_height_target_m) + double(rear_delta)) *
        39.3699989 * 4.0 + 0.5);
    s.corner[1].ride_height_target_m = float(
        std::nearbyint(double(rf_steps)) * 0.0253999997 * 0.25);
}

SimFile SimFile::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || !match_tag(bytes, 0, "PGTS")) {
        throw std::runtime_error("SimFile: not a PGTS file");
    }
    SimFile out;
    out.pgts_version = read_u32(bytes, 4);
    std::uint32_t pgts_size = read_u32(bytes, 8);

    std::size_t pgts_end = std::min<std::size_t>(12 + pgts_size, bytes.size());
    std::size_t pos = 12;

    while (pos + 12 <= pgts_end) {
        pos = skip_padding(bytes, pos, pgts_end);
        if (pos + 12 > pgts_end) break;

        // Read inner sub-chunk.
        std::uint32_t version = read_u32(bytes, pos + 4);
        std::uint32_t size    = read_u32(bytes, pos + 8);
        std::size_t   body    = pos + 12;
        std::size_t   bend    = body + size;
        if (bend > pgts_end) {
            throw std::runtime_error("SimFile: sub-chunk extends past PGTS body");
        }

        if (match_tag(bytes, pos, "HGTS")) {
            out.hgts_version = version;
            out.raw_hgts.assign(bytes.begin() + body, bytes.begin() + bend);
            // Find the embedded name. The body starts with a couple of
            // metadata bytes then a NUL-terminated path-like string.
            std::size_t scan = body;
            // Skip a small header (variable, observed 1-2 bytes before '*').
            while (scan < bend && bytes[scan] != '*' && scan - body < 16) ++scan;
            if (scan < bend && bytes[scan] == '*') ++scan;
            // Read name until NUL.
            std::size_t name_start = scan;
            while (scan < bend && bytes[scan] != 0) ++scan;
            out.embedded_name.assign(reinterpret_cast<const char*>(&bytes[name_start]),
                                      scan - name_start);
        } else if (match_tag(bytes, pos, "DGTS")) {
            out.dgts_version = version;
            out.setup = parse_dgts(std::span<const std::uint8_t>(&bytes[body], size));
        } else if (match_tag(bytes, pos, "TGTS")) {
            out.raw_tgts.assign(bytes.begin() + body, bytes.begin() + bend);
            // Strip trailing NULs.
            std::size_t text_end = bend;
            while (text_end > body && bytes[text_end - 1] == 0) --text_end;
            out.description.assign(reinterpret_cast<const char*>(&bytes[body]),
                                    text_end - body);
        }
        // Unknown sub-chunks ignored.

        pos = bend;
    }
    return out;
}

namespace {

void put_f32(std::vector<std::uint8_t>& b, std::size_t off, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    b[off + 0] = static_cast<std::uint8_t>(bits);
    b[off + 1] = static_cast<std::uint8_t>(bits >> 8);
    b[off + 2] = static_cast<std::uint8_t>(bits >> 16);
    b[off + 3] = static_cast<std::uint8_t>(bits >> 24);
}

void put_i32(std::vector<std::uint8_t>& b, std::size_t off, std::int32_t v) {
    const std::uint32_t bits = static_cast<std::uint32_t>(v);
    b[off + 0] = static_cast<std::uint8_t>(bits);
    b[off + 1] = static_cast<std::uint8_t>(bits >> 8);
    b[off + 2] = static_cast<std::uint8_t>(bits >> 16);
    b[off + 3] = static_cast<std::uint8_t>(bits >> 24);
}

// Rebuild the 272-byte DGTS body, patching every typed field over a copy
// of raw_dgts (so edited fields AND still-unmapped bytes are correct).
// Offsets mirror parse_dgts one-for-one.  RF ride height is not on disk
// (parse_dgts reconstructs it), so it is deliberately not written.
std::vector<std::uint8_t> build_dgts_body(const CarSetup& s) {
    std::vector<std::uint8_t> b = s.raw_dgts;
    if (b.size() < 0x110) b.assign(0x110, 0);
    for (int c = 0; c < 4; ++c) {
        put_f32(b, 0x000 + c * 4, s.corner[c].tire_pressure);
        put_i32(b, 0x010 + c * 4, s.corner[c].bump_low);
        put_i32(b, 0x020 + c * 4, s.corner[c].bump_high);
        put_i32(b, 0x030 + c * 4, s.corner[c].rebound_low);
        put_i32(b, 0x040 + c * 4, s.corner[c].rebound_high);
        put_f32(b, 0x050 + c * 4, s.corner[c].spring_rate);
        put_f32(b, 0x060 + c * 4, s.corner[c].spring_rubber);
        put_f32(b, 0x070 + c * 4, s.corner[c].spring_transition_width_m);
        put_f32(b, 0x080 + c * 4, s.corner[c].camber);
    }
    put_f32(b, 0x090, s.corner[0].ride_height_target_m);  // LF
    put_f32(b, 0x094, s.corner[2].ride_height_target_m);  // LR
    put_f32(b, 0x098, s.corner[3].ride_height_target_m);  // RR
    put_f32(b, 0x09c, s.brake_bias);
    put_f32(b, 0x0a0, s.front_toe_out);
    put_f32(b, 0x0a4, s.rear_toe_out);
    put_i32(b, 0x0a8, s.front_bar_setting);
    put_i32(b, 0x0ac, s.rear_bar_setting);
    put_f32(b, 0x0b0, s.rear_track_bar_lr);
    put_f32(b, 0x0b4, s.rear_track_bar_rr);
    put_i32(b, 0x0b8, s.unknown_0b8);
    put_f32(b, 0x0bc, s.steer_ratio);
    put_f32(b, 0x0c0, s.fuel_load_kg);
    put_f32(b, 0x0e8, s.grille_tape);
    put_f32(b, 0x0ec, s.unknown_0ec);
    // Inverse of parse_dgts's round(stored + 0.1) + 45 display transform.
    // Leave the disk bytes untouched for an unset (zero) spoiler so a
    // never-populated setup doesn't write a bogus -45.
    if (s.rear_spoiler_angle_deg != 0.0f)
        put_f32(b, 0x0f0, s.rear_spoiler_angle_deg - 45.0f);
    put_f32(b, 0x0f4, s.left_right_bias);
    put_f32(b, 0x0f8, s.front_rear_bias);
    put_f32(b, 0x0fc, s.wedge);
    put_f32(b, 0x100, s.front_stagger_radius_delta_m);
    put_f32(b, 0x104, s.rear_stagger_radius_delta_m);
    put_f32(b, 0x108, s.front_caster_lf);
    put_f32(b, 0x10c, s.front_caster_rf);
    // Drivetrain gear + final-drive indices.  Gears 5/6 (slots 0x45/0x46) are
    // past the 272-byte body and deliberately not written, so a save stays
    // byte-exact.  These values already live in raw_dgts, so re-emitting them
    // is a no-op for an unedited setup and preserves round-trip fidelity.
    put_i32(b, 0x0c8, s.gear_count);
    put_i32(b, 0x0cc, s.reverse_index);
    put_i32(b, 0x0d0, s.gear_index[0]);
    put_i32(b, 0x0d4, s.gear_index[1]);
    put_i32(b, 0x0d8, s.gear_index[2]);
    put_i32(b, 0x0dc, s.gear_index[3]);
    put_i32(b, 0x0e0, s.final_drive_index);
    return b;
}

// The HGTS body holds two NUL-terminated copies of the embedded name: one
// just after the '*' delimiter near the start, one at +0x102.  Overwrite
// both, then NUL-terminate (residual bytes past the NUL are ignored by the
// reader).  Used only when saving under a new name.
void patch_hgts_name(std::vector<std::uint8_t>& h, std::string_view name) {
    auto write_copy = [&](std::size_t start, std::size_t limit) {
        if (start >= limit) return;
        const std::size_t n = std::min(name.size(), limit - start - 1);
        for (std::size_t i = 0; i < n; ++i)
            h[start + i] = static_cast<std::uint8_t>(name[i]);
        h[start + n] = 0;
    };
    std::size_t star = 0;
    while (star < h.size() && star < 16 && h[star] != '*') ++star;
    if (star < h.size() && h[star] == '*')
        write_copy(star + 1, std::min<std::size_t>(0x100, h.size()));
    if (h.size() > 0x102) write_copy(0x102, h.size());
}

// Minimal valid HGTS body for a setup that was never parsed from disk.
// The two leading bytes' meaning is undetermined (observed values vary:
// 0x2e, 0x56, 0xc7); 0x56/0x00 is one real combination, used here as an
// approximation.  Fixed 514-byte size matches every shipped cup.sim.
std::vector<std::uint8_t> synth_hgts(std::string_view name) {
    std::vector<std::uint8_t> h(514, 0);
    h[0] = 0x56;
    h[1] = 0x00;
    h[2] = 0x2a;  // '*'
    patch_hgts_name(h, name.empty() ? std::string_view("setup.cup.sim") : name);
    return h;
}

void append_chunk(std::vector<std::uint8_t>& out, const char* tag,
                  std::uint32_t version, const std::vector<std::uint8_t>& body) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(tag[i]));
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(version >> (8 * i)));
    const std::uint32_t sz = static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(sz >> (8 * i)));
    out.insert(out.end(), body.begin(), body.end());
}

}  // namespace

std::vector<std::uint8_t> SimFile::write(std::string_view new_embedded_name) const {
    std::vector<std::uint8_t> hgts = raw_hgts.empty()
        ? synth_hgts(new_embedded_name.empty() ? std::string_view(embedded_name)
                                               : new_embedded_name)
        : raw_hgts;
    if (!raw_hgts.empty() && !new_embedded_name.empty())
        patch_hgts_name(hgts, new_embedded_name);

    const std::vector<std::uint8_t> dgts = build_dgts_body(setup);

    std::vector<std::uint8_t> tgts;
    if (!raw_tgts.empty()) {
        tgts = raw_tgts;
    } else {
        tgts.assign(description.begin(), description.end());
        tgts.push_back(0);
        if (tgts.size() < 2048) tgts.resize(2048, 0);
    }

    // Sub-chunks in shipped order: HGTS, two-space pad, DGTS, TGTS.
    std::vector<std::uint8_t> sub;
    append_chunk(sub, "HGTS", hgts_version ? hgts_version : 3u, hgts);
    sub.push_back(0x20);
    sub.push_back(0x20);
    append_chunk(sub, "DGTS", dgts_version ? dgts_version : 13u, dgts);
    append_chunk(sub, "TGTS", 1u, tgts);

    std::vector<std::uint8_t> out;
    const char* pg = "PGTS";
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(pg[i]));
    const std::uint32_t pv = pgts_version ? pgts_version : 5u;
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(pv >> (8 * i)));
    const std::uint32_t sz = static_cast<std::uint32_t>(sub.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(sz >> (8 * i)));
    out.insert(out.end(), sub.begin(), sub.end());
    return out;
}

}  // namespace opennr
