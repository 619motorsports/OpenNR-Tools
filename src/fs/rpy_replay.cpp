#include "rpy_replay.h"

#include "core/byte_reader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace opennr {

namespace {
float decode_papyrus_tangent_code(std::int32_t code, float shape,
                                  float range, std::int32_t max_code) noexcept {
    const float limit_angle = std::atan(1.0f / std::sqrt(shape));
    const float value_scale = std::tan(limit_angle) / range;
    const float code_scale = limit_angle / (static_cast<float>(max_code) + 0.5f);
    return std::tan(static_cast<float>(code) * code_scale) / value_scale;
}
} // namespace

float RpyRptpType11DebrisState::lateral() const noexcept {
    return decode_papyrus_tangent_code(lateral_code, 128.0f, 300.0f, 65535);
}

float RpyRptpType11DebrisState::height_above_ground() const noexcept {
    return decode_papyrus_tangent_code(height_code, 1024.0f, 25.0f, 511);
}

std::array<float, 3> RpyRptpType11DebrisState::orientation_rad() const noexcept {
    constexpr float kOrientationScale = 0.02463921532034874f;
    return {orientation_codes[0] * kOrientationScale,
            orientation_codes[1] * kOrientationScale,
            orientation_codes[2] * kOrientationScale};
}

Vec3 RpyRptpType11DebrisState::renderer_orientation_xyz() const noexcept {
    const auto zyx = orientation_rad();
    return {zyx[2], zyx[1], zyx[0]};
}

namespace {

// FourCC tags in .rpy are written byte-reversed on disk: the logical
// "RPLY" appears as 'Y','L','P','R'. Each chunk has the same 12-byte
// prologue: tag(4), version(u32), body_size(u32). NB: the field at +4
// is a *version*, not the always-zero "reserved" word that the .stp/
// .mip/.sim parsers tolerate. So its outer value (5 for stock files)
// is meaningful and surfaced in RpyReplay::outer_version.
constexpr std::array<char, 4> on_disk_tag(const char (&logical)[5]) {
    return {logical[3], logical[2], logical[1], logical[0]};
}

const auto kTagRPLY = on_disk_tag("RPLY");
const auto kTagRPHD = on_disk_tag("RPHD");
const auto kTagWKNF = on_disk_tag("WKNF");
const auto kTagDRLS = on_disk_tag("DRLS");
const auto kTagDRNT = on_disk_tag("DRNT");
const auto kTagLPTB = on_disk_tag("LPTB");
const auto kTagLPRO = on_disk_tag("LPRO");
const auto kTagRPTP = on_disk_tag("RPTP");
const auto kTagRPRS = on_disk_tag("RPRS");

bool tag_equals(std::span<const std::uint8_t> bytes, std::size_t pos,
                const std::array<char, 4>& tag) {
    if (pos + 4 > bytes.size()) return false;
    return bytes[pos    ] == static_cast<std::uint8_t>(tag[0]) &&
           bytes[pos + 1] == static_cast<std::uint8_t>(tag[1]) &&
           bytes[pos + 2] == static_cast<std::uint8_t>(tag[2]) &&
           bytes[pos + 3] == static_cast<std::uint8_t>(tag[3]);
}

// Sibling chunks may be separated by up to 3 NUL or 0x20 padding bytes,
// matching the convention seen in .sim and .stp.
std::size_t skip_padding(std::span<const std::uint8_t> buf,
                          std::size_t pos, std::size_t end) {
    std::size_t padded = 0;
    while (pos < end && padded < 3 && (buf[pos] == 0 || buf[pos] == 0x20)) {
        ++pos;
        ++padded;
    }
    return pos;
}

std::uint32_t read_u32(std::span<const std::uint8_t> b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

std::uint16_t read_u16(std::span<const std::uint8_t> b, std::size_t off) {
    return static_cast<std::uint16_t>(b[off]) |
           (static_cast<std::uint16_t>(b[off + 1]) << 8);
}

// Trim a fixed-width NUL-padded ASCII slot to a std::string.
std::string read_slot(std::span<const std::uint8_t> b, std::size_t off, std::size_t cap) {
    std::size_t n = 0;
    while (n < cap && off + n < b.size() && b[off + n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(&b[off]), n);
}

std::string read_utf16le_z(std::span<const std::uint8_t> b, std::size_t off) {
    std::string out;
    while (off + 1 < b.size()) {
        std::uint32_t code = read_u16(b, off);
        off += 2;
        if (code == 0) break;
        if (code >= 0xd800 && code <= 0xdbff && off + 1 < b.size()) {
            const std::uint32_t low = read_u16(b, off);
            if (low >= 0xdc00 && low <= 0xdfff) {
                off += 2;
                code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
            }
        }
        if (code <= 0x7f) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }
    return out;
}

RpyHeader parse_rphd(std::span<const std::uint8_t> body) {
    RpyHeader h;
    h.raw.assign(body.begin(), body.end());
    if (body.size() >= 4)  h.frame_count    = read_u32(body, 0x00);
    if (body.size() >= 8)  h.rptp_body_size = read_u32(body, 0x04);
    if (body.size() >= 12) h.event_count    = read_u32(body, 0x08);
    if (body.size() >= 16) h.reserved_0c    = read_u32(body, 0x0c);
    if (body.size() >= 0x18) h.player_first = read_slot(body, 0x10, 8);
    if (body.size() >= 0x28) h.player_last  = read_slot(body, 0x20, 8);
    if (body.size() >= 0x3c) h.reserved_38  = read_u32(body, 0x38);
    if (body.size() > 0x3c) h.summary = read_utf16le_z(body, 0x3c);
    return h;
}

// Read a misaligned f32 starting at `off` in `body`.  The session
// table at WKNF+0x6c stores each duration_seconds field at
// record offset 3 (byte 3..7 of an 8-byte record) which is NOT
// 4-byte aligned, so we read the 4 bytes manually rather than
// reinterpret_cast.
float read_f32_unaligned(std::span<const std::uint8_t> b, std::size_t off) {
    std::uint32_t u =
         static_cast<std::uint32_t>(b[off])         |
        (static_cast<std::uint32_t>(b[off + 1]) <<  8) |
        (static_cast<std::uint32_t>(b[off + 2]) << 16) |
        (static_cast<std::uint32_t>(b[off + 3]) << 24);
    float f;
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

std::int16_t read_i16(std::span<const std::uint8_t> b, std::size_t off) {
    return static_cast<std::int16_t>(
         static_cast<std::uint16_t>(b[off]) |
        (static_cast<std::uint16_t>(b[off + 1]) << 8));
}

RpySessionEntry parse_session_entry(std::span<const std::uint8_t> b,
                                    std::size_t off) {
    RpySessionEntry e;
    e.flags_lo         = b[off + 0];
    e.lap_limit        = read_i16(b, off + 1);
    e.duration_seconds = read_f32_unaligned(b, off + 3);
    e.type_tag         = b[off + 7];
    return e;
}

RpyWeekend parse_wknf(std::span<const std::uint8_t> body) {
    RpyWeekend w;
    w.raw.assign(body.begin(), body.end());
    if (body.size() >= 0x20) w.track_name = read_slot(body, 0x00, 0x20);
    if (body.size() >= 0x40) w.class_name = read_slot(body, 0x20, 0x20);
    if (body.size() >= 0x44) w.session_id      = read_u32(body, 0x40);
    if (body.size() >= 0x5d) w.temperature_f   = body[0x5c];
    if (body.size() >= 0x64) w.race_length_pct = read_u32(body, 0x60);
    if (body.size() >= 0x67) w.num_ai          = body[0x66];
    // 4 session entries at +0x6c, each 8 bytes.  Need 0x6c + 4*8 = 0x8c bytes.
    if (body.size() >= 0x8c) {
        for (std::size_t i = 0; i < 4; ++i) {
            w.sessions[i] = parse_session_entry(body, 0x6c + i * 8);
        }
    }
    return w;
}

RpyDriverEntry parse_drnt(std::span<const std::uint8_t> body) {
    RpyDriverEntry d;
    d.raw.assign(body.begin(), body.end());
    if (body.size() >= 0x10) d.first_name = read_slot(body, 0x00, 0x10);
    if (body.size() >= 0x20) d.last_name  = read_slot(body, 0x10, 0x10);
    if (body.size() >= 0x90) d.car_file   = read_slot(body, 0x70, 0x20);
    if (body.size() >= 0x94) d.mfg_index   = read_u32(body, 0x90);
    if (body.size() >= 0x98) d.flags_a     = read_u32(body, 0x94);
    if (body.size() >= 0x9c) d.reserved_88 = read_u32(body, 0x98);
    if (body.size() >= 0xa0) d.car_number  = read_u32(body, 0x9c);
    if (body.size() >= 0xa4) d.skill_or_xp = read_u32(body, 0xa0);
    if (body.size() >= 0xa8) d.list_id     = read_u32(body, 0xa4);
    if (body.size() >= 0xac) d.race_pos    = read_u32(body, 0xa8);
    if (body.size() >= 0xaf) {
        d.tail[0] = body[0xac];
        d.tail[1] = body[0xad];
        d.tail[2] = body[0xae];
        d.ai_controlled = (body[0xac] != 0);
        d.is_racer      = (body[0xad] != 0);
        d.player_flags  = body[0xae];
    }
    return d;
}

RpyLapRow parse_lpro(std::span<const std::uint8_t> body) {
    RpyLapRow r;
    r.raw.assign(body.begin(), body.end());
    if (body.size() >= 4) r.car_index   = read_u32(body, 0);
    if (body.size() >= 8) r.field_at_4  = read_u32(body, 4);
    for (std::size_t i = 0; i < r.lap_times_ms.size(); ++i) {
        std::size_t off = 8 + i * 4;
        if (off + 4 <= body.size()) {
            r.lap_times_ms[i] = read_u32(body, off);
        }
    }
    return r;
}

// Walk one chunk header and return (tag, version, body_size, body_off, end).
struct ChunkHeader {
    std::size_t   header_off;   // offset of the 12-byte header
    std::array<char, 4> tag;    // on-disk tag bytes
    std::uint32_t version;
    std::uint32_t body_size;
    std::size_t   body_off;     // = header_off + 12
    std::size_t   end_off;      // = body_off + body_size
};

ChunkHeader read_header(std::span<const std::uint8_t> bytes, std::size_t pos) {
    ChunkHeader h;
    h.header_off = pos;
    h.tag = {static_cast<char>(bytes[pos]),
             static_cast<char>(bytes[pos + 1]),
             static_cast<char>(bytes[pos + 2]),
             static_cast<char>(bytes[pos + 3])};
    h.version   = read_u32(bytes, pos + 4);
    h.body_size = read_u32(bytes, pos + 8);
    h.body_off  = pos + 12;
    h.end_off   = h.body_off + h.body_size;
    return h;
}

}  // namespace

namespace {

std::int32_t sign_extend(std::uint32_t value, unsigned bits) {
    const std::uint32_t sign = 1u << (bits - 1u);
    return static_cast<std::int32_t>((value ^ sign) - sign);
}

float decode_tangent(std::int32_t code, float curve, float limit,
                     unsigned bits, bool is_signed) {
    const auto max_code = float((1u << (bits - (is_signed ? 1u : 0u))) - 1u);
    const float root = std::sqrt(curve);
    return std::tan(float(code) * (std::atan(root) / max_code)) * (limit / root);
}

RpyCarSample decode_car_sample(std::span<const std::uint8_t> b) {
    RpyCarSample c;
    if (b.size() < 16) return c;
    std::copy_n(b.begin(), 16, c.raw.begin());
    const std::uint16_t w0 = std::uint16_t(b[0]) | (std::uint16_t(b[1]) << 8);
    const std::uint16_t w1 = std::uint16_t(b[2]) | (std::uint16_t(b[3]) << 8);
    const std::uint32_t d1 = read_u32(b, 4);
    const std::uint32_t d2 = read_u32(b, 8);
    const std::uint32_t d3 = read_u32(b, 12);
    c.status_bit       = (w0 & 1u) != 0;
    c.tick             = (w0 >> 1) & 0x03ffu;
    c.throttle_code    = static_cast<std::uint8_t>((w0 >> 11) & 0x1fu);
    c.pitch_code       = static_cast<std::int16_t>(sign_extend(w1 & 0x03ffu, 10));
    c.steering_code    = static_cast<std::int8_t>(sign_extend((w1 >> 10) & 0x3fu, 6));
    c.along_track_code = d1 & 0x00ffffffu;
    c.car_index        = static_cast<std::uint8_t>((d1 >> 24) & 0x7fu);
    c.discontinuity    = (d1 & 0x80000000u) != 0;
    c.roll_code        = static_cast<std::int16_t>(sign_extend(d2 & 0x03ffu, 10));
    c.rpm_code         = static_cast<std::uint16_t>((d2 >> 10) & 0x03ffu);
    c.state_flag       = (d2 & 0x00100000u) != 0;
    c.vertical_code    = static_cast<std::uint16_t>((d2 >> 21) & 0x01ffu);
    c.mode             = static_cast<std::uint8_t>((d2 >> 30) & 0x03u);
    c.yaw_code         = static_cast<std::int16_t>(sign_extend(d3 & 0x0fffu, 12));
    c.lateral_code     = sign_extend((d3 >> 12) & 0x1ffffu, 17);
    c.status_flags     = static_cast<std::uint8_t>((d3 >> 29) & 0x07u);
    return c;
}

}  // namespace

Vec3 RpyMarkerExtensionEntry::base_position() const noexcept {
    return {float(x_fixed) / 128.0f,
            float(y_fixed) / 128.0f,
            float(z_fixed) / 128.0f};
}

std::array<RpyMarkerExtensionPoint, 3>
RpyMarkerExtensionEntry::auxiliary_vertices() const noexcept {
    std::array<RpyMarkerExtensionPoint, 3> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::size_t off = i * 4;
        const std::uint32_t word =
            std::uint32_t(packed[off]) |
            (std::uint32_t(packed[off + 1]) << 8) |
            (std::uint32_t(packed[off + 2]) << 16) |
            (std::uint32_t(packed[off + 3]) << 24);
        auto& point = out[i];
        point.x_offset_code = static_cast<std::int16_t>(
            sign_extend(word & 0x3ffu, 10));
        point.y_offset_code = static_cast<std::int16_t>(
            sign_extend((word >> 10) & 0x3ffu, 10));
        point.z_offset_code = static_cast<std::int8_t>(
            sign_extend((word >> 20) & 0xffu, 8));
        point.intensity_nibble = static_cast<std::uint8_t>(word >> 28);
        const auto addFixed = [](std::int32_t value, std::int32_t offset) {
            // FUN_005b53e0 performs the integer add before FILD/scaling.
            return static_cast<std::int32_t>(
                static_cast<std::uint32_t>(value) +
                static_cast<std::uint32_t>(offset));
        };
        point.world_position = {
            float(addFixed(x_fixed, point.x_offset_code)) / 128.0f,
            float(addFixed(y_fixed, point.y_offset_code)) / 128.0f,
            float(addFixed(z_fixed, point.z_offset_code)) / 128.0f,
        };
    }
    return out;
}

std::optional<RpyMarkerExtensionEntry> encode_rpy_tire_mark_entry(
    const RpyTireMarkQuadState& state,
    std::uint32_t frameIndex) noexcept {
    std::array<std::array<std::int32_t, 3>, 4> fixed{};
    for (std::size_t i = 0; i < fixed.size(); ++i) {
        const std::array<float, 3> values{
            state.vertices[i].x, state.vertices[i].y, state.vertices[i].z};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(values[axis])) return std::nullopt;
            const double biased = double(values[axis]) +
                (axis == 2 ? double(std::bit_cast<float>(0x3c75c28fu)) : 0.0);
            const double scaled = biased * 128.0;
            if (scaled < double(std::numeric_limits<std::int32_t>::min()) ||
                scaled > double(std::numeric_limits<std::int32_t>::max())) {
                return std::nullopt;
            }
            fixed[i][axis] = static_cast<std::int32_t>(scaled); // x87 chop
        }
    }

    RpyMarkerExtensionEntry out;
    out.x_fixed = fixed[0][0];
    out.y_fixed = fixed[0][1];
    out.z_fixed = fixed[0][2];
    out.flags = std::uint16_t(state.vertex_intensity_bytes[0] >> 4);
    out.object_id = state.object_id;
    out.frame_index = frameIndex;

    for (std::size_t i = 0; i < 3; ++i) {
        const std::int64_t dx = std::int64_t(fixed[i + 1][0]) - fixed[0][0];
        const std::int64_t dy = std::int64_t(fixed[i + 1][1]) - fixed[0][1];
        const std::int64_t dz = std::int64_t(fixed[i + 1][2]) - fixed[0][2];
        if (dx < -512 || dx > 511 || dy < -512 || dy > 511 ||
            dz < -128 || dz > 127) {
            return std::nullopt;
        }
        const std::uint32_t word =
            (std::uint32_t(dx) & 0x3ffu) |
            ((std::uint32_t(dy) & 0x3ffu) << 10) |
            ((std::uint32_t(dz) & 0xffu) << 20) |
            (std::uint32_t(state.vertex_intensity_bytes[i + 1] >> 4) << 28);
        const std::size_t off = i * 4;
        out.packed[off] = std::uint8_t(word);
        out.packed[off + 1] = std::uint8_t(word >> 8);
        out.packed[off + 2] = std::uint8_t(word >> 16);
        out.packed[off + 3] = std::uint8_t(word >> 24);
    }
    return out;
}

float RpyCarSample::along_track(float track_length) const noexcept {
    constexpr float kMax24 = 16777215.0f;
    const float scale = track_length > 0.0f
        ? std::max(1.0f / 1024.0f, (track_length + 10.0f) / kMax24)
        : 1.0f / 1024.0f;
    return float(along_track_code) * scale;
}

float RpyCarSample::lateral() const noexcept {
    return decode_tangent(lateral_code, 128.0f, 300.0f, 17, true);
}

float RpyCarSample::vertical() const noexcept {
    return decode_tangent(vertical_code, 1024.0f, 25.0f, 9, false);
}

float RpyCarSample::yaw_rad() const noexcept {
    return float(yaw_code) * std::bit_cast<float>(0x3ac92900u);
}

float RpyCarSample::pitch_rad() const noexcept {
    constexpr float kHalfPi = 1.57079632679489661923f;
    return decode_tangent(pitch_code, 16.0f, kHalfPi, 10, true);
}

float RpyCarSample::roll_rad() const noexcept {
    constexpr float kPi = 3.14159265358979323846f;
    return decode_tangent(roll_code, 16.0f, kPi, 10, true);
}

float RpyCarSample::engine_rpm() const noexcept {
    return decode_tangent(rpm_code, 16.0f, 32000.0f, 10, false);
}

float RpyCarSample::throttle() const noexcept {
    return decode_tangent(throttle_code, 12.0f, 1.0f, 5, false);
}

float RpyCarSample::steering_rad() const noexcept {
    constexpr float kPiOverSix = 0.52359877559829887308f;
    return decode_tangent(steering_code, 128.0f, kPiOverSix, 6, true);
}

RpyFrameSamples RpyReplay::samples_at_frame(std::uint32_t frame_index) const {
    RpyFrameSamples out;
    out.frame_index = frame_index;
    auto marker = markers.end();
    for (auto it = markers.begin(); it != markers.end(); ++it) {
        if (it->frame_index > frame_index) continue;
        if (marker == markers.end() || it->frame_index > marker->frame_index)
            marker = it;
    }
    if (marker == markers.end()) return out;
    out.frame_index = marker->frame_index;

    const auto& frame = frame_blocks[marker->frame_block];
    const std::size_t event_end = std::size_t(frame.first_event) + frame.event_count;
    for (std::size_t i = marker->event_index + 1; i < event_end; ++i) {
        const auto& event = events[i];
        if (event.type != 3) continue;
        const std::size_t sample_count = (event.logical_size - 4u) / 16u;
        const std::size_t sample_offset = std::size_t(event.body_offset) + 4u;
        out.cars.reserve(out.cars.size() + sample_count);
        for (std::size_t sample = 0; sample < sample_count; ++sample) {
            out.cars.push_back(decode_car_sample(std::span<const std::uint8_t>(
                frame_data.data() + sample_offset + sample * 16u, 16u)));
        }
    }
    return out;
}

RpyFrameSamples RpyReplay::samples_at_block(std::uint32_t block_index) const {
    RpyFrameSamples out;
    out.frame_index = block_index;
    if (block_index >= frame_blocks.size()) return out;
    const auto& frame = frame_blocks[block_index];
    const std::size_t event_end = std::size_t(frame.first_event) + frame.event_count;
    for (std::size_t i = frame.first_event; i < event_end; ++i) {
        const auto& event = events[i];
        if (event.type == 14 && event.logical_size >= 8)
            out.frame_index = read_u32(frame_data, event.body_offset + 4u);
        if (event.type != 3) continue;
        const std::size_t sample_count = (event.logical_size - 4u) / 16u;
        const std::size_t sample_offset = std::size_t(event.body_offset) + 4u;
        out.cars.reserve(out.cars.size() + sample_count);
        for (std::size_t sample = 0; sample < sample_count; ++sample) {
            out.cars.push_back(decode_car_sample(std::span<const std::uint8_t>(
                frame_data.data() + sample_offset + sample * 16u, 16u)));
        }
    }
    return out;
}

std::span<const RpyRptpType11DebrisState>
RpyReplay::debris_at_block(std::uint32_t block_index) const noexcept {
    if (block_index >= rptp_type11_blocks.size()) return {};
    const auto range = rptp_type11_blocks[block_index];
    if (range.first_state > rptp_type11_debris.size() ||
        range.state_count > rptp_type11_debris.size() - range.first_state) {
        return {};
    }
    return {rptp_type11_debris.data() + range.first_state, range.state_count};
}

RpyReplay RpyReplay::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12) {
        throw std::runtime_error("RpyReplay: file shorter than chunk header");
    }
    if (!tag_equals(bytes, 0, kTagRPLY)) {
        throw std::runtime_error("RpyReplay: missing RPLY magic (expected "
                                  "byte-reversed 'YLPR')");
    }
    auto outer = read_header(bytes, 0);
    if (outer.end_off > bytes.size()) {
        throw std::runtime_error("RpyReplay: RPLY body overruns file");
    }

    RpyReplay out;
    out.outer_version = outer.version;

    std::size_t pos = outer.body_off;
    bool saw_rptp = false;
    int  lptb_index = 0;

    while (pos + 12 <= outer.end_off) {
        pos = skip_padding(bytes, pos, outer.end_off);
        if (pos + 12 > outer.end_off) break;
        auto sub = read_header(bytes, pos);
        if (sub.end_off > outer.end_off) {
            throw std::runtime_error("RpyReplay: sub-chunk extends past RPLY body");
        }
        auto body = bytes.subspan(sub.body_off, sub.body_size);

        if (tag_equals(bytes, pos, kTagRPHD)) {
            out.header = parse_rphd(body);
        } else if (tag_equals(bytes, pos, kTagWKNF)) {
            out.weekend = parse_wknf(body);
        } else if (tag_equals(bytes, pos, kTagDRLS)) {
            // Walk inner DRNT children.
            std::size_t inner = sub.body_off;
            while (inner + 12 <= sub.end_off) {
                inner = skip_padding(bytes, inner, sub.end_off);
                if (inner + 12 > sub.end_off) break;
                auto child = read_header(bytes, inner);
                if (child.end_off > sub.end_off)
                    throw std::runtime_error("RpyReplay: DRLS child overruns container");
                if (tag_equals(bytes, inner, kTagDRNT)) {
                    out.drivers.push_back(parse_drnt(
                        bytes.subspan(child.body_off, child.body_size)));
                }
                inner = child.end_off;
            }
        } else if (tag_equals(bytes, pos, kTagLPTB)) {
            // Walk inner LPRO children. Even when the LPTB body is empty we
            // still bump the table index so the four-table layout is preserved.
            if (lptb_index < static_cast<int>(out.lap_tables.size())) {
                auto& tbl = out.lap_tables[lptb_index];
                std::size_t inner = sub.body_off;
                while (inner + 12 <= sub.end_off) {
                    inner = skip_padding(bytes, inner, sub.end_off);
                    if (inner + 12 > sub.end_off) break;
                    auto child = read_header(bytes, inner);
                    if (child.end_off > sub.end_off)
                        throw std::runtime_error("RpyReplay: LPTB child overruns container");
                    if (tag_equals(bytes, inner, kTagLPRO)) {
                        tbl.rows.push_back(parse_lpro(
                            bytes.subspan(child.body_off, child.body_size)));
                    }
                    inner = child.end_off;
                }
            }
            ++lptb_index;
        } else if (tag_equals(bytes, pos, kTagRPTP)) {
            if (saw_rptp)
                throw std::runtime_error("RpyReplay: duplicate RPTP chunk");
            // RPTP is a sequence of exactly RPHD.frame_count frame blocks.
            if (out.header.rptp_body_size != 0 &&
                out.header.rptp_body_size != sub.body_size) {
                throw std::runtime_error(
                    "RpyReplay: RPHD rptp_body_size disagrees with RPTP body size");
            }
            out.frame_data.assign(body.begin(), body.end());

            // Decode the 28-byte session header at the start of the body.
            // session_id at +0x14 mirrors WKNF.session_id so the WKNF/RPTP
            // pair can be verified consistent.
            if (out.frame_data.size() >= 28) {
                std::copy_n(out.frame_data.begin(), 28,
                            out.rptp_header.raw.begin());
                out.rptp_header.session_id =
                    read_u32(out.frame_data, 0x14);
            }

            std::size_t frame_offset = 0;
            std::uint16_t previous_size = 0;
            out.frame_blocks.reserve(out.header.frame_count);
            out.rptp_type11_blocks.reserve(out.header.frame_count);
            for (std::uint32_t block = 0; block < out.header.frame_count; ++block) {
                if (frame_offset + 8 > out.frame_data.size())
                    throw std::runtime_error("RpyReplay: truncated RPTP frame header");

                RpyRptpFrameIndex frame;
                frame.body_offset = static_cast<std::uint32_t>(frame_offset);
                frame.total_size = read_u16(out.frame_data, frame_offset);
                frame.previous_frame_size = read_u16(out.frame_data, frame_offset + 2);
                frame.sequence = read_u16(out.frame_data, frame_offset + 4);
                frame.flags = read_u16(out.frame_data, frame_offset + 6);
                frame.first_event = static_cast<std::uint32_t>(out.events.size());
                const auto type11_first =
                    static_cast<std::uint32_t>(out.rptp_type11_debris.size());
                if (frame.total_size < 8)
                    throw std::runtime_error("RpyReplay: RPTP frame is smaller than its header");
                if (block != 0 && frame.previous_frame_size != previous_size)
                    throw std::runtime_error("RpyReplay: broken RPTP previous-frame-size chain");
                const std::size_t frame_end = frame_offset + frame.total_size;
                if (frame_end > out.frame_data.size())
                    throw std::runtime_error("RpyReplay: RPTP frame overruns body");

                std::size_t event_offset = frame_offset + 8;
                while (event_offset < frame_end) {
                    if (event_offset + 4 > frame_end)
                        throw std::runtime_error("RpyReplay: truncated RPTP event header");
                    RpyRptpEventIndex event;
                    event.body_offset = static_cast<std::uint32_t>(event_offset);
                    event.type = read_u16(out.frame_data, event_offset);
                    event.logical_size = read_u16(out.frame_data, event_offset + 2);
                    if (event.logical_size < 4)
                        throw std::runtime_error("RpyReplay: RPTP event is smaller than its header");
                    const std::size_t stride = (std::size_t(event.logical_size) + 3u) & ~std::size_t(3u);
                    if (stride > frame_end - event_offset)
                        throw std::runtime_error("RpyReplay: RPTP event overruns frame");
                    if (event.type == 14 &&
                        (event.logical_size < 8 || (event.logical_size - 8) % 35 != 0))
                        throw std::runtime_error("RpyReplay: malformed type-14 event size");
                    if (event.type == 3 &&
                        (event.logical_size < 4 || (event.logical_size - 4) % 16 != 0))
                        throw std::runtime_error("RpyReplay: malformed type-3 event size");
                    if (event.type == 3) {
                        const std::size_t count = (event.logical_size - 4u) / 16u;
                        for (std::size_t sample = 0; sample < count; ++sample) {
                            const std::size_t sample_offset = event_offset + 4u + sample * 16u;
                            if ((out.frame_data[sample_offset + 7u] & 0x7fu) >= 48u)
                                throw std::runtime_error("RpyReplay: type-3 car index exceeds 47");
                        }
                    }
                    if (event.type == 11 && event.logical_size != 20)
                        throw std::runtime_error("RpyReplay: malformed type-11 debris event size");

                    const auto event_index = static_cast<std::uint32_t>(out.events.size());
                    out.events.push_back(event);
                    ++frame.event_count;
                    if (event.type == 14) {
                        RpyRptpMarkerIndex marker;
                        marker.frame_index = read_u32(out.frame_data, event_offset + 4);
                        marker.frame_block = block;
                        marker.event_index = event_index;
                        out.markers.push_back(marker);
                        if (out.marker_count == 0) out.first_frame_index = marker.frame_index;
                        out.last_frame_index = marker.frame_index;
                        ++out.marker_count;
                        ++out.rptp_counts.type14_marker;
                        // Extension entries: tire-mark quads in 35-byte
                        // records after the frame index. Size validity was
                        // enforced above.
                        if (event.logical_size > 8) {
                            RpyMarkerExtension ext;
                            ext.frame_index = marker.frame_index;
                            ext.frame_block = block;
                            const std::size_t n =
                                (event.logical_size - 8u) / 35u;
                            for (std::size_t k = 0; k < n; ++k) {
                                const std::size_t eo =
                                    event_offset + 8u + k * 35u;
                                RpyMarkerExtensionEntry e;
                                const auto x = read_u32(out.frame_data, eo);
                                const auto y = read_u32(out.frame_data, eo + 4);
                                const auto z = read_u32(out.frame_data, eo + 8);
                                e.x_fixed = static_cast<std::int32_t>(x);
                                e.y_fixed = static_cast<std::int32_t>(y);
                                e.z_fixed = static_cast<std::int32_t>(z);
                                e.flags = read_u16(out.frame_data, eo + 12);
                                std::copy_n(out.frame_data.begin() + eo + 14,
                                            12, e.packed.begin());
                                e.object_id = out.frame_data[eo + 26];
                                e.reserved =
                                    read_u32(out.frame_data, eo + 27);
                                e.frame_index =
                                    read_u32(out.frame_data, eo + 31);
                                ext.entries.push_back(e);
                                ++out.rptp_counts.type14_extension_entries;
                            }
                            out.marker_extensions.push_back(std::move(ext));
                        }
                    } else if (event.type == 11) {
                        const auto d0 = read_u32(out.frame_data, event_offset + 4);
                        const auto d1 = read_u32(out.frame_data, event_offset + 8);
                        const auto d2 = read_u32(out.frame_data, event_offset + 12);
                        RpyRptpType11DebrisState debris;
                        debris.body_offset = event.body_offset;
                        debris.frame_block = block;
                        debris.pool_slot = static_cast<std::uint8_t>(d0 & 0x1fu);
                        debris.generation_id = (d0 >> 5) & 0x3ffffu;
                        debris.height_code = static_cast<std::uint16_t>(d0 >> 23);
                        debris.kind = static_cast<std::uint8_t>(d1 & 3u);
                        auto lateral = static_cast<std::int32_t>((d1 >> 2) & 0x1ffffu);
                        if ((lateral & 0x10000) != 0) lateral -= 0x20000;
                        debris.lateral_code = lateral;
                        debris.along_track_code = d2 & 0xffffffu;
                        debris.orientation_codes = {
                            out.frame_data[event_offset + 15],
                            out.frame_data[event_offset + 16],
                            out.frame_data[event_offset + 17]};
                        debris.appearance = read_u16(out.frame_data, event_offset + 18);
                        out.rptp_type11_debris.push_back(debris);
                    } else if (event.type == 9) {
                        ++out.rptp_counts.type9_kinematic;
                        if (event.logical_size >= 8) {
                            const auto state_code = read_u32(out.frame_data, event_offset + 4);
                            if (state_code == 2) ++out.rptp_counts.type9_state_code_2;
                            else if (state_code == 1) ++out.rptp_counts.type9_state_code_1;
                            else if (state_code == 5) ++out.rptp_counts.type9_state_code_5;
                            else ++out.rptp_counts.type9_state_code_other;
                        }
                    } else if (event.type == 13) {
                        ++out.rptp_counts.type13_event;
                    } else if (event.type == 4) {
                        ++out.rptp_counts.type4_session_init;
                        if (event.logical_size >= 20) {
                            RpyRptpType4 cp;
                            cp.body_offset = event.body_offset;
                            cp.sub_tag = out.frame_data[event_offset + 4];
                            std::copy_n(out.frame_data.begin() + event_offset + 5, 3,
                                        cp.state_bytes.begin());
                            const auto st = read_u32(out.frame_data, event_offset + 8);
                            std::memcpy(&cp.sim_time, &st, sizeof(float));
                            cp.session_id = read_u32(out.frame_data, event_offset + 12);
                            cp.reserved = read_u32(out.frame_data, event_offset + 16);
                            out.rptp_type4_checkpoints.push_back(cp);
                        }
                    } else if (event.type == 3) {
                        ++out.rptp_counts.type3_large_state;
                    }
                    event_offset += stride;
                }
                if (event_offset != frame_end)
                    throw std::runtime_error("RpyReplay: RPTP events do not consume frame");
                const auto type11_count = out.rptp_type11_debris.size() - type11_first;
                if (type11_count > std::numeric_limits<std::uint16_t>::max())
                    throw std::runtime_error("RpyReplay: too many type-11 states in one frame");
                out.rptp_type11_blocks.push_back({
                    type11_first, static_cast<std::uint16_t>(type11_count)});
                out.frame_blocks.push_back(frame);
                previous_size = frame.total_size;
                frame_offset = frame_end;
            }
            if (frame_offset != out.frame_data.size())
                throw std::runtime_error("RpyReplay: RPTP frames do not consume body");
            out.rptp_bytes_consumed = static_cast<std::uint32_t>(frame_offset);
            saw_rptp = true;
        } else if (tag_equals(bytes, pos, kTagRPRS)) {
            // Trailer / sentinel — body_size always 0 in stock files. Stop
            // walking once we see it.
            (void)sub;
        }
        // Any other tag is silently skipped; future passes can decode
        // extension chunks without breaking this parser.

        pos = sub.end_off;
    }

    if (!saw_rptp) {
        throw std::runtime_error("RpyReplay: missing RPTP per-frame chunk");
    }

    return out;
}

}  // namespace opennr
