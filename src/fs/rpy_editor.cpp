#include "rpy_editor.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace opennr {
namespace {

std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t p) {
    return std::uint16_t(b[p]) | (std::uint16_t(b[p + 1]) << 8);
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t p) {
    return std::uint32_t(b[p]) | (std::uint32_t(b[p + 1]) << 8) |
           (std::uint32_t(b[p + 2]) << 16) | (std::uint32_t(b[p + 3]) << 24);
}
void put16(std::vector<std::uint8_t>& b, std::size_t p, std::uint16_t v) {
    b[p] = std::uint8_t(v); b[p + 1] = std::uint8_t(v >> 8);
}
void put32(std::vector<std::uint8_t>& b, std::size_t p, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b[p + i] = std::uint8_t(v >> (8 * i));
}
void append16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(std::uint8_t(v)); b.push_back(std::uint8_t(v >> 8));
}
void append32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b.push_back(std::uint8_t(v >> (8 * i)));
}
void append_float(std::vector<std::uint8_t>& b, float v) {
    const auto n = std::bit_cast<std::uint32_t>(v);
    for (unsigned i = 0; i < 4; ++i) b.push_back(std::uint8_t(n >> (8 * i)));
}
float get_float(std::span<const std::uint8_t> b, std::size_t p) {
    return std::bit_cast<float>(u32(b, p));
}
bool editor_type(std::uint16_t type) { return type >= 19 && type <= 28; }

const char* type_name(RpyEditorEventType type) {
    static constexpr const char* names[] = {"Stamp", "Sound", "Text", "Fade",
        "Camera", "Car", "Playback", "Marker", "Toggle", "Volume"};
    const auto n = static_cast<std::uint16_t>(type);
    return editor_type(n) ? names[n - 19] : "Unknown";
}
bool ascii_z(std::span<const std::uint8_t> b, std::size_t start) {
    if (start >= b.size() || b.back() != 0) return false;
    for (std::size_t i = start; i + 1 < b.size(); ++i)
        if (b[i] == 0 || b[i] > 0x7f) return false;
    return true;
}
bool valid_ascii(const std::string& s) {
    for (unsigned char c : s) if (c == 0 || c > 0x7f) return false;
    return true;
}
std::string ascii_at(std::span<const std::uint8_t> b, std::size_t p) {
    return std::string(reinterpret_cast<const char*>(b.data() + p), b.size() - p - 1);
}

struct Layout {
    std::size_t outer_end, rphd_body, rphd_end, rptp_header, rptp_body, rptp_end;
};
bool tag(std::span<const std::uint8_t> b, std::size_t p, const char* s) {
    return p + 4 <= b.size() && b[p] == std::uint8_t(s[3]) &&
        b[p + 1] == std::uint8_t(s[2]) && b[p + 2] == std::uint8_t(s[1]) &&
        b[p + 3] == std::uint8_t(s[0]);
}
Layout layout(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || !tag(bytes, 0, "RPLY"))
        throw std::runtime_error("RpyEditor: missing RPLY chunk");
    const std::size_t outer_end = 12ull + u32(bytes, 8);
    if (outer_end > bytes.size()) throw std::runtime_error("RpyEditor: RPLY overruns file");
    Layout l{outer_end, 0, 0, 0, 0, 0};
    unsigned rphd = 0, rptp = 0;
    std::size_t p = 12;
    while (p < outer_end) {
        unsigned pad = 0;
        while (p < outer_end && pad < 3 && (bytes[p] == 0 || bytes[p] == 0x20)) { ++p; ++pad; }
        if (p == outer_end) break;
        if (p + 12 > outer_end) throw std::runtime_error("RpyEditor: truncated chunk header");
        const std::size_t end = p + 12ull + u32(bytes, p + 8);
        if (end > outer_end) throw std::runtime_error("RpyEditor: chunk overruns RPLY");
        if (tag(bytes, p, "RPHD")) {
            ++rphd;
            l.rphd_body = p + 12;
            l.rphd_end = end;
        }
        if (tag(bytes, p, "RPTP")) { ++rptp; l.rptp_header = p; l.rptp_body = p + 12; l.rptp_end = end; }
        p = end;
    }
    if (rphd != 1 || rptp != 1) throw std::runtime_error("RpyEditor: missing or duplicate RPHD/RPTP chunk");
    if (l.rphd_body + 8 > outer_end) throw std::runtime_error("RpyEditor: RPHD is too short");
    return l;
}

std::vector<std::uint8_t> record(RpyEditorEventType type,
                                 std::span<const std::uint8_t> payload) {
    if (!validate_rpy_editor_payload(type, payload))
        throw std::invalid_argument("RpyEditor: invalid editor event payload");
    if (payload.size() > std::numeric_limits<std::uint16_t>::max() - 4u)
        throw std::invalid_argument("RpyEditor: event is too large");
    std::vector<std::uint8_t> out;
    append16(out, static_cast<std::uint16_t>(type));
    append16(out, static_cast<std::uint16_t>(payload.size() + 4));
    out.insert(out.end(), payload.begin(), payload.end());
    out.resize((out.size() + 3u) & ~std::size_t(3u), 0);
    return out;
}

enum class Edit { Insert, Delete, Replace };
std::vector<std::uint8_t> rebuild(std::span<const std::uint8_t> original, Edit edit,
                                  std::uint32_t index, RpyEditorEventType type,
                                  std::span<const std::uint8_t> payload) {
    const auto old = RpyReplay::parse(original);
    const auto loc = layout(original);
    if (old.header.frame_count != old.frame_blocks.size())
        throw std::runtime_error("RpyEditor: incomplete frame index");
    std::vector<std::uint8_t> replacement;
    if (edit != Edit::Delete) replacement = record(type, payload);
    std::uint32_t target_frame = index;
    if (edit != Edit::Insert) {
        if (index >= old.events.size()) throw std::out_of_range("RpyEditor: event index out of range");
        if (!editor_type(old.events[index].type))
            throw std::invalid_argument("RpyEditor: event is not an editor event");
        for (std::uint32_t f = 0; f < old.frame_blocks.size(); ++f) {
            const auto& frame = old.frame_blocks[f];
            if (index >= frame.first_event && index < frame.first_event + frame.event_count) {
                target_frame = f; break;
            }
        }
    } else if (target_frame >= old.frame_blocks.size()) {
        throw std::out_of_range("RpyEditor: frame index out of range");
    }

    std::vector<std::uint8_t> body;
    std::uint16_t previous = 0;
    for (std::uint32_t f = 0; f < old.frame_blocks.size(); ++f) {
        const auto& frame = old.frame_blocks[f];
        const auto frame_start = body.size();
        body.insert(body.end(), old.frame_data.begin() + frame.body_offset,
                    old.frame_data.begin() + frame.body_offset + 8);
        put16(body, frame_start + 2, previous);
        bool inserted = false;
        const auto end = frame.first_event + frame.event_count;
        for (std::uint32_t e = frame.first_event; e < end; ++e) {
            const auto& ev = old.events[e];
            if (edit == Edit::Insert && f == target_frame && !editor_type(ev.type) && !inserted) {
                body.insert(body.end(), replacement.begin(), replacement.end()); inserted = true;
            }
            if (f == target_frame && e == index && edit != Edit::Insert) {
                if (edit == Edit::Replace) body.insert(body.end(), replacement.begin(), replacement.end());
                continue;
            }
            const std::size_t stride = (std::size_t(ev.logical_size) + 3u) & ~std::size_t(3u);
            body.insert(body.end(), old.frame_data.begin() + ev.body_offset,
                        old.frame_data.begin() + ev.body_offset + stride);
        }
        if (edit == Edit::Insert && f == target_frame && !inserted)
            body.insert(body.end(), replacement.begin(), replacement.end());
        const auto size = body.size() - frame_start;
        if (size > std::numeric_limits<std::uint16_t>::max())
            throw std::overflow_error("RpyEditor: frame exceeds UINT16_MAX");
        previous = static_cast<std::uint16_t>(size);
        put16(body, frame_start, previous);
    }
    if (body.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("RpyEditor: RPTP exceeds UINT32_MAX");

    std::vector<std::uint8_t> out;
    out.reserve(original.size() - (loc.rptp_end - loc.rptp_body) + body.size());
    out.insert(out.end(), original.begin(), original.begin() + loc.rptp_body);
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), original.begin() + loc.rptp_end, original.end());
    const auto delta = std::ptrdiff_t(body.size()) - std::ptrdiff_t(loc.rptp_end - loc.rptp_body);
    const auto shifted = [delta, &loc](std::size_t p) { return p > loc.rptp_header ? std::size_t(std::ptrdiff_t(p) + delta) : p; };
    put32(out, shifted(loc.rptp_header) + 8, static_cast<std::uint32_t>(body.size()));
    put32(out, shifted(loc.rphd_body) + 4, static_cast<std::uint32_t>(body.size()));
    put32(out, 8, static_cast<std::uint32_t>(std::ptrdiff_t(u32(original, 8)) + delta));
    (void)RpyReplay::parse(out);
    return out;
}

std::vector<std::uint8_t> rebuild_type11_fixtures(
    std::span<const std::uint8_t> original,
    std::span<const RpyType11FixtureEntry> entries) {
    const auto old = RpyReplay::parse(original);
    const auto loc = layout(original);
    if (old.header.frame_count != old.frame_blocks.size())
        throw std::runtime_error("RpyEditor: incomplete frame index");

    std::vector<std::vector<std::uint8_t>> additions(old.frame_blocks.size());
    for (const auto& entry : entries) {
        if (entry.frame_block >= additions.size())
            throw std::out_of_range("RpyEditor: fixture frame index out of range");
        const auto payload = encode_rpy_type11_fixture_payload(entry.state);
        auto& record = additions[entry.frame_block];
        append16(record, 11);
        append16(record, static_cast<std::uint16_t>(payload.size() + 4u));
        record.insert(record.end(), payload.begin(), payload.end());
    }

    std::vector<std::uint8_t> body;
    std::uint16_t previous = 0;
    for (std::uint32_t f = 0; f < old.frame_blocks.size(); ++f) {
        const auto& frame = old.frame_blocks[f];
        const auto frame_start = body.size();
        body.insert(body.end(), old.frame_data.begin() + frame.body_offset,
                    old.frame_data.begin() + frame.body_offset + 8);
        put16(body, frame_start + 2, previous);
        const auto end = frame.first_event + frame.event_count;
        for (std::uint32_t e = frame.first_event; e < end; ++e) {
            const auto& event = old.events[e];
            const std::size_t stride =
                (std::size_t(event.logical_size) + 3u) & ~std::size_t(3u);
            body.insert(body.end(), old.frame_data.begin() + event.body_offset,
                        old.frame_data.begin() + event.body_offset + stride);
        }
        body.insert(body.end(), additions[f].begin(), additions[f].end());
        const auto size = body.size() - frame_start;
        if (size > std::numeric_limits<std::uint16_t>::max())
            throw std::overflow_error("RpyEditor: frame exceeds UINT16_MAX");
        previous = static_cast<std::uint16_t>(size);
        put16(body, frame_start, previous);
    }
    if (body.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("RpyEditor: RPTP exceeds UINT32_MAX");

    std::vector<std::uint8_t> out;
    out.reserve(original.size() - (loc.rptp_end - loc.rptp_body) + body.size());
    out.insert(out.end(), original.begin(), original.begin() + loc.rptp_body);
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), original.begin() + loc.rptp_end, original.end());
    const auto delta = std::ptrdiff_t(body.size()) -
        std::ptrdiff_t(loc.rptp_end - loc.rptp_body);
    const auto shifted = [delta, &loc](std::size_t p) {
        return p > loc.rptp_header ? std::size_t(std::ptrdiff_t(p) + delta) : p;
    };
    put32(out, shifted(loc.rptp_header) + 8, static_cast<std::uint32_t>(body.size()));
    put32(out, shifted(loc.rphd_body) + 4, static_cast<std::uint32_t>(body.size()));
    put32(out, 8, static_cast<std::uint32_t>(std::ptrdiff_t(u32(original, 8)) + delta));
    (void)RpyReplay::parse(out);
    return out;
}

}  // namespace

bool validate_rpy_editor_payload(RpyEditorEventType type,
                                 std::span<const std::uint8_t> p) noexcept {
    switch (type) {
    case RpyEditorEventType::Stamp: return p.size() >= 14 && ascii_z(p, 13);
    case RpyEditorEventType::Sound: return p.size() >= 2 && ascii_z(p, 1);
    case RpyEditorEventType::Text:
        if (p.size() < 17 || ((p.size() - 15) & 1) || p[p.size()-2] || p.back()) return false;
        for (std::size_t i = 15; i + 2 < p.size(); i += 2) if (!p[i] && !p[i+1]) return false;
        return true;
    case RpyEditorEventType::Fade: return p.size() == 5;
    case RpyEditorEventType::Camera: case RpyEditorEventType::Car: return p.size() == 2;
    case RpyEditorEventType::Playback: return p.size() == 6;
    case RpyEditorEventType::Marker: return p.empty();
    case RpyEditorEventType::Toggle: return p.size() == 3;
    case RpyEditorEventType::Volume: return p.size() == 12;
    }
    return false;
}

std::string decode_rpy_editor_payload(RpyEditorEventType type,
                                      std::span<const std::uint8_t> p) {
    if (!validate_rpy_editor_payload(type, p)) throw std::invalid_argument("RpyEditor: invalid editor event payload");
    std::ostringstream s; s << type_name(type);
    switch (type) {
    case RpyEditorEventType::Stamp: s << ": " << ascii_at(p, 13); break;
    case RpyEditorEventType::Sound: s << ": " << ascii_at(p, 1) << " [" << unsigned(p[0]) << ']'; break;
    case RpyEditorEventType::Text: {
        s << ": ";
        for (std::size_t i=15; i+2<p.size(); i+=2) {
            const auto c = u16(p, i); s << (c >= 0x20 && c <= 0x7e ? char(c) : '?');
        } break;
    }
    case RpyEditorEventType::Fade: s << ": " << get_float(p,0) << ", " << unsigned(p[4]); break;
    case RpyEditorEventType::Camera: case RpyEditorEventType::Car: s << ": " << unsigned(p[0]) << ", " << unsigned(p[1]); break;
    case RpyEditorEventType::Playback: s << ": " << get_float(p,0) << ", " << unsigned(p[4]) << ", " << unsigned(p[5]); break;
    case RpyEditorEventType::Marker: break;
    case RpyEditorEventType::Toggle: s << ": " << unsigned(p[0]) << ", " << unsigned(p[1]) << ", " << unsigned(p[2]); break;
    case RpyEditorEventType::Volume: s << ": " << get_float(p,0) << ", " << get_float(p,4) << ", " << get_float(p,8); break;
    }
    return s.str();
}

std::vector<RpyEditorOperation> enumerate_rpy_editor_operations(const RpyReplay& r) {
    std::vector<RpyEditorOperation> out;
    for (std::uint32_t f=0; f<r.frame_blocks.size(); ++f) {
        const auto& frame=r.frame_blocks[f];
        for (std::uint32_t i=frame.first_event; i<frame.first_event+frame.event_count; ++i) {
            const auto& e=r.events[i]; if (!editor_type(e.type)) continue;
            const auto p=std::span<const std::uint8_t>(r.frame_data).subspan(e.body_offset+4,e.logical_size-4);
            out.push_back({static_cast<RpyEditorEventType>(e.type),f,i,decode_rpy_editor_payload(static_cast<RpyEditorEventType>(e.type),p)});
        }
    }
    return out;
}

RpyEditorCameraState replay_editor_camera_state(
    const RpyReplay& replay, std::uint32_t through_frame) {
    RpyEditorCameraState state;
    apply_replay_editor_camera_events(replay, 0, through_frame, state);
    return state;
}

void apply_replay_editor_camera_events(
    const RpyReplay& replay, std::uint32_t first_frame,
    std::uint32_t through_frame, RpyEditorCameraState& state) {
    if (replay.frame_blocks.empty() || first_frame >= replay.frame_blocks.size())
        return;
    const auto end_frame = std::min<std::uint32_t>(
        through_frame, static_cast<std::uint32_t>(replay.frame_blocks.size() - 1));
    for (std::uint32_t frame = first_frame; frame <= end_frame; ++frame) {
        const auto& block = replay.frame_blocks[frame];
        for (std::uint32_t index = block.first_event;
             index < block.first_event + block.event_count; ++index) {
            const auto& event = replay.events[index];
            const auto payload = std::span<const std::uint8_t>(replay.frame_data)
                .subspan(event.body_offset + 4, event.logical_size - 4);
            if (payload.size() != 2) continue;
            if (event.type == static_cast<std::uint16_t>(
                    RpyEditorEventType::Camera)) {
                state.camera = payload[0];
            } else if (event.type == static_cast<std::uint16_t>(
                           RpyEditorEventType::Car)) {
                state.car = payload[0];
            }
        }
    }
}

std::vector<std::uint8_t> make_rpy_stamp_payload(const std::array<std::uint8_t,13>& x,const std::string& n){if(!valid_ascii(n))throw std::invalid_argument("RpyEditor: invalid ASCII");std::vector<std::uint8_t> r(x.begin(),x.end());r.insert(r.end(),n.begin(),n.end());r.push_back(0);return r;}
std::vector<std::uint8_t> make_rpy_stamp_payload(const RpyStampPayload& v) {
    if (!valid_ascii(v.name)) throw std::invalid_argument("RpyEditor: invalid ASCII");
    std::vector<std::uint8_t> r;
    // The native modal stores Fade Time first and Life Span second.  The
    // public struct keeps the user-facing field names, so do not serialize
    // the declaration order by accident.
    append_float(r,v.fade_time); append_float(r,v.lifespan);
    append16(r,v.x); append16(r,v.y); r.push_back(v.lesson_resource ? 1 : 0);
    r.insert(r.end(),v.name.begin(),v.name.end()); r.push_back(0); return r;
}
std::vector<std::uint8_t> make_rpy_sound_payload(std::uint8_t f,const std::string& n){if(!valid_ascii(n))throw std::invalid_argument("RpyEditor: invalid ASCII");std::vector<std::uint8_t> r{f};r.insert(r.end(),n.begin(),n.end());r.push_back(0);return r;}
std::vector<std::uint8_t> make_rpy_text_payload(const std::array<std::uint8_t,15>& x,const std::u16string& n){std::vector<std::uint8_t> r(x.begin(),x.end());for(char16_t c:n){if(!c)throw std::invalid_argument("RpyEditor: embedded NUL");append16(r,c);}append16(r,0);return r;}
std::vector<std::uint8_t> make_rpy_text_payload(const RpyTextPayload& v) {
    std::vector<std::uint8_t> r;
    r.reserve(17 + v.text.size() * 2);
    // As with Stamp, the native text modal writes Fade Time before Life
    // Span, even though the dialog presents both as named fields.
    append_float(r, v.fade_time);
    append_float(r, v.lifespan);
    append16(r, v.x);
    append16(r, v.y);
    append16(r, v.max_pixel_width);
    r.push_back(v.enabled ? 1 : 0);
    for (char16_t c : v.text) {
        if (!c) throw std::invalid_argument("RpyEditor: embedded NUL");
        append16(r, c);
    }
    append16(r, 0);
    return r;
}
std::vector<std::uint8_t> make_rpy_fade_payload(float v,std::uint8_t b){std::vector<std::uint8_t> r;append_float(r,v);r.push_back(b);return r;}
std::vector<std::uint8_t> make_rpy_fade_payload(const RpyFadePayload& v) {
    return make_rpy_fade_payload(
        v.lifespan, std::uint8_t((v.fade_in ? 1u : 0u) |
                                 (v.previous_fade_in ? 2u : 0u)));
}
std::vector<std::uint8_t> make_rpy_camera_payload(std::uint8_t a,std::uint8_t b){return {a,b};}
std::vector<std::uint8_t> make_rpy_car_payload(std::uint8_t a,std::uint8_t b){return {a,b};}
std::vector<std::uint8_t> make_rpy_playback_payload(float v,std::uint8_t a,std::uint8_t b){std::vector<std::uint8_t> r;append_float(r,v);r.push_back(a);r.push_back(b);return r;}
std::vector<std::uint8_t> make_rpy_playback_payload(const RpyPlaybackPayload& v) {
    if (v.rate_index > 16 || (v.slow_motion && v.rate_index == 0))
        throw std::invalid_argument("RpyEditor: invalid playback rate");
    std::vector<std::uint8_t> r;
    append_float(r, v.lifespan);
    const auto adjusted = std::uint8_t(v.rate_index - (v.slow_motion ? 1 : 0));
    r.push_back(std::uint8_t((adjusted << 1) | (v.slow_motion ? 1 : 0)));
    r.push_back(v.previous_packed_rate);
    return r;
}
std::vector<std::uint8_t> make_rpy_marker_payload(){return {};}
std::vector<std::uint8_t> make_rpy_toggle_payload(std::uint8_t a,std::uint8_t b,std::uint8_t c){return {a,b,c};}
std::vector<std::uint8_t> make_rpy_toggle_payload(const RpyTogglePayload& v) {
    if (v.item > 2) throw std::invalid_argument("RpyEditor: invalid toggle item");
    return {v.item, std::uint8_t(v.enabled), std::uint8_t(v.previous_enabled)};
}
std::vector<std::uint8_t> make_rpy_volume_payload(float a,float b,float c){std::vector<std::uint8_t> r;append_float(r,a);append_float(r,b);append_float(r,c);return r;}
std::vector<std::uint8_t> make_rpy_volume_payload(const RpyVolumePayload& v) {
    std::vector<std::uint8_t> r;
    append_float(r, v.lifespan);
    append_float(r, v.volume);
    append_float(r, v.previous_volume);
    return r;
}

std::optional<RpyTextPayload> parse_rpy_text_payload(
    std::span<const std::uint8_t> p) {
    if (!validate_rpy_editor_payload(RpyEditorEventType::Text, p)) return std::nullopt;
    RpyTextPayload v;
    v.fade_time = get_float(p, 0);
    v.lifespan = get_float(p, 4);
    v.x = u16(p, 8);
    v.y = u16(p, 10);
    v.max_pixel_width = u16(p, 12);
    v.enabled = p[14] != 0;
    for (std::size_t at = 15; at + 2 < p.size(); at += 2)
        v.text.push_back(char16_t(u16(p, at)));
    return v;
}

std::optional<RpyStampPayload> parse_rpy_stamp_payload(
    std::span<const std::uint8_t> p) {
    if (!validate_rpy_editor_payload(RpyEditorEventType::Stamp,p)) return std::nullopt;
    return RpyStampPayload{get_float(p,0),get_float(p,4),u16(p,8),u16(p,10),
                           (p[12]&1u)!=0,ascii_at(p,13)};
}

std::optional<RpySoundPayload> parse_rpy_sound_payload(
    std::span<const std::uint8_t> p) {
    if (!validate_rpy_editor_payload(RpyEditorEventType::Sound, p))
        return std::nullopt;
    RpySoundPayload v;
    v.flags = p[0];
    v.lesson_resource = (p[0] & 1u) != 0;
    v.name = ascii_at(p, 1);
    return v;
}

std::optional<RpyFadePayload> parse_rpy_fade_payload(
    std::span<const std::uint8_t> p) {
    if (!validate_rpy_editor_payload(RpyEditorEventType::Fade, p))
        return std::nullopt;
    return RpyFadePayload{get_float(p, 0), (p[4] & 1u) != 0,
                          (p[4] & 2u) != 0};
}

std::optional<RpyPlaybackPayload> parse_rpy_playback_payload(
    std::span<const std::uint8_t> p) {
    if (!validate_rpy_editor_payload(RpyEditorEventType::Playback, p))
        return std::nullopt;
    RpyPlaybackPayload v;
    v.lifespan = get_float(p, 0);
    v.slow_motion = (p[4] & 1u) != 0;
    v.rate_index = std::uint8_t((p[4] >> 1) + (v.slow_motion ? 1 : 0));
    v.previous_packed_rate = p[5];
    if (v.rate_index > 16) return std::nullopt;
    return v;
}

std::optional<RpyTogglePayload> parse_rpy_toggle_payload(
    std::span<const std::uint8_t> p) {
    if (!validate_rpy_editor_payload(RpyEditorEventType::Toggle, p) || p[0] > 2 ||
        p[1] > 1 || p[2] > 1) return std::nullopt;
    return RpyTogglePayload{p[0], p[1] != 0, p[2] != 0};
}

std::optional<RpyVolumePayload> parse_rpy_volume_payload(
    std::span<const std::uint8_t> p) {
    if (!validate_rpy_editor_payload(RpyEditorEventType::Volume, p))
        return std::nullopt;
    return RpyVolumePayload{get_float(p, 0), get_float(p, 4), get_float(p, 8)};
}

std::vector<std::uint8_t> replace_rpy_summary(
    std::span<const std::uint8_t> original, const std::u16string& summary) {
    const auto loc = layout(original);
    constexpr std::size_t offset = 0x3c;
    if (loc.rphd_end < loc.rphd_body + offset + 2)
        throw std::runtime_error("RpyEditor: RPHD has no summary field");
    const auto capacity = (loc.rphd_end - (loc.rphd_body + offset)) / 2;
    if (summary.size() + 1 > capacity)
        throw std::invalid_argument("RpyEditor: summary is too long");
    std::vector<std::uint8_t> out(original.begin(), original.end());
    const auto begin = loc.rphd_body + offset;
    std::fill(out.begin() + begin, out.begin() + loc.rphd_end, 0);
    for (std::size_t index = 0; index < summary.size(); ++index) {
        if (!summary[index]) throw std::invalid_argument("RpyEditor: embedded NUL");
        put16(out, begin + index * 2, summary[index]);
    }
    (void)RpyReplay::parse(out);
    return out;
}

std::vector<std::uint8_t> insert_rpy_editor_event(std::span<const std::uint8_t> b,std::uint32_t f,RpyEditorEventType t,std::span<const std::uint8_t> p){return rebuild(b,Edit::Insert,f,t,p);}
std::vector<std::uint8_t> delete_rpy_editor_event(std::span<const std::uint8_t> b,std::uint32_t i){return rebuild(b,Edit::Delete,i,RpyEditorEventType::Marker,{});}
std::vector<std::uint8_t> replace_rpy_editor_event(std::span<const std::uint8_t> b,std::uint32_t i,RpyEditorEventType t,std::span<const std::uint8_t> p){return rebuild(b,Edit::Replace,i,t,p);}

std::array<std::uint8_t, 16> encode_rpy_type11_fixture_payload(
    const RpyRptpType11DebrisState& state) {
    if (state.pool_slot > 31 || state.generation_id > 0x3ffffu ||
        state.height_code > 0x1ffu || (state.kind != 1 && state.kind != 2) ||
        state.lateral_code < -0x10000 || state.lateral_code > 0xffff ||
        state.along_track_code > 0xffffffu || state.car_slot() > 0x7fu ||
        state.panel_id() > 0x0fu || state.paint_id() > 7 ||
        state.appearance_reserved() > 3) {
        throw std::invalid_argument("RpyEditor: type-11 fixture value is out of range");
    }
    std::vector<std::uint8_t> payload;
    payload.reserve(16);
    append32(payload, std::uint32_t(state.pool_slot) |
                      (state.generation_id << 5) |
                      (std::uint32_t(state.height_code) << 23));
    append32(payload, std::uint32_t(state.kind) |
                      ((std::uint32_t(state.lateral_code) & 0x1ffffu) << 2));
    // The third dword carries the 24-bit track code followed by the first
    // raw Z/Y/X orientation byte; the remaining two bytes precede appearance.
    append32(payload, state.along_track_code |
                      (std::uint32_t(state.orientation_codes[0]) << 24));
    payload.push_back(state.orientation_codes[1]);
    payload.push_back(state.orientation_codes[2]);
    append16(payload, state.appearance);
    std::array<std::uint8_t, 16> out{};
    std::copy(payload.begin(), payload.end(), out.begin());
    return out;
}

std::vector<std::uint8_t> insert_rpy_type11_fixture_events(
    std::span<const std::uint8_t> original,
    std::span<const RpyType11FixtureEntry> entries) {
    return rebuild_type11_fixtures(original, entries);
}

}  // namespace opennr
