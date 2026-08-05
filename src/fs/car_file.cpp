#include "car_file.h"

#include "core/byte_reader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string_view>

namespace opennr {

namespace {

// Papyrus FourCCs are stored as little-endian u32, so a tag we display
// as "CARF" hits disk as 'F','R','A','C'.  The helper compares the 4
// bytes at `pos` against the *display* form.
bool match_fourcc(std::span<const std::uint8_t> buf, std::size_t pos, const char* fourcc) {
    if (pos + 4 > buf.size()) return false;
    return buf[pos]     == static_cast<std::uint8_t>(fourcc[3]) &&
           buf[pos + 1] == static_cast<std::uint8_t>(fourcc[2]) &&
           buf[pos + 2] == static_cast<std::uint8_t>(fourcc[1]) &&
           buf[pos + 3] == static_cast<std::uint8_t>(fourcc[0]);
}

// Step over up to 3 bytes of 0x20 padding before the next chunk header.
std::size_t skip_padding(std::span<const std::uint8_t> buf, std::size_t pos) {
    std::size_t orig = pos;
    while (pos < buf.size() && buf[pos] == 0x20 && pos - orig < 3) {
        // If the next four bytes already look like a tag (4 ASCII letters),
        // we've crossed into the next chunk and should stop.
        if (pos + 4 <= buf.size()) {
            bool four_ascii = true;
            for (int i = 0; i < 4; ++i) {
                std::uint8_t b = buf[pos + i];
                if (b < 0x20 || b >= 0x7F) { four_ascii = false; break; }
            }
            if (four_ascii && buf[pos] != 0x20) break;
        }
        ++pos;
    }
    return pos;
}

void trim(std::string& s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
}

void parse_ini(const std::string& text, std::map<std::string, IniSection>& out) {
    std::string current;
    std::size_t i = 0;
    while (i < text.size()) {
        // Read one line (CRLF or LF).
        std::size_t line_end = text.find('\n', i);
        std::string line = text.substr(i, (line_end == std::string::npos ? text.size() : line_end) - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        i = (line_end == std::string::npos) ? text.size() : line_end + 1;

        std::string trimmed = line;
        trim(trimmed);
        if (trimmed.empty()) continue;

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            current = trimmed.substr(1, trimmed.size() - 2);
            out[current];  // create empty section if missing
            continue;
        }
        if (current.empty()) continue;
        std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key   = trimmed.substr(0, eq);
        std::string value = trimmed.substr(eq + 1);
        trim(key);
        trim(value);
        out[current][key] = value;
    }
}

}  // namespace

CarFile CarFile::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12) {
        throw std::runtime_error("CarFile: file shorter than CARF header");
    }
    if (!match_fourcc(bytes, 0, "CARF")) {
        throw std::runtime_error("CarFile: missing CARF magic");
    }

    ByteReader r(bytes);
    r.skip(4);                     // 'CARF' tag
    if (r.read_u32_le() != 0) {    // reserved
        // Tolerate; some tools may write garbage here. Don't fail.
    }
    std::uint32_t carf_size = r.read_u32_le();
    std::size_t carf_end = std::min<std::size_t>(12 + carf_size, bytes.size());

    CarFile out;
    std::size_t pos = 12;

    while (pos + 12 <= carf_end) {
        pos = skip_padding(bytes, pos);
        if (pos + 12 > carf_end) break;

        // Read the 12-byte sub-chunk header.
        std::uint32_t reserved = static_cast<std::uint32_t>(bytes[pos + 4]) |
                                  (static_cast<std::uint32_t>(bytes[pos + 5]) << 8) |
                                  (static_cast<std::uint32_t>(bytes[pos + 6]) << 16) |
                                  (static_cast<std::uint32_t>(bytes[pos + 7]) << 24);
        (void)reserved;
        std::uint32_t size = static_cast<std::uint32_t>(bytes[pos + 8]) |
                              (static_cast<std::uint32_t>(bytes[pos + 9]) << 8) |
                              (static_cast<std::uint32_t>(bytes[pos + 10]) << 16) |
                              (static_cast<std::uint32_t>(bytes[pos + 11]) << 24);
        std::size_t body_start = pos + 12;
        std::size_t body_end   = body_start + size;
        if (body_end > carf_end) {
            throw std::runtime_error("CarFile: sub-chunk extends past CARF body");
        }

        if (match_fourcc(bytes, pos, "CTYP")) {
            if (size != 4) throw std::runtime_error("CarFile: CTYP body must be 4 bytes");
            out.ctyp = static_cast<std::uint32_t>(bytes[body_start]) |
                       (static_cast<std::uint32_t>(bytes[body_start + 1]) << 8) |
                       (static_cast<std::uint32_t>(bytes[body_start + 2]) << 16) |
                       (static_cast<std::uint32_t>(bytes[body_start + 3]) << 24);
        } else if (match_fourcc(bytes, pos, "CINI")) {
            std::string text(reinterpret_cast<const char*>(&bytes[body_start]), size);
            parse_ini(text, out.ini);
        } else if (match_fourcc(bytes, pos, "CBIO")) {
            out.cbio_bytes.assign(bytes.begin() + body_start, bytes.begin() + body_end);
        } else if (match_fourcc(bytes, pos, "PHOT")) {
            out.phot_bytes.assign(bytes.begin() + body_start, bytes.begin() + body_end);
        } else if (match_fourcc(bytes, pos, "CTEX")) {
            out.ctex_bytes.assign(bytes.begin() + body_start, bytes.begin() + body_end);
        } else if (match_fourcc(bytes, pos, "CREW")) {
            out.crew_bytes.assign(bytes.begin() + body_start, bytes.begin() + body_end);
        }
        // Unknown tags are silently skipped to keep us forward-compatible
        // with future Papyrus chunk types we haven't seen.

        pos = body_end;
    }

    return out;
}

std::vector<std::uint8_t> CarFile::serialize() const {
    static constexpr std::array<std::string_view, 15> ai_keys = {{
        "aiParamDriverAggression", "aiParamDriverConsistency",
        "aiParamDriverFinishing", "aiParamDriverQualifying",
        "aiParamDriverRoadCourse", "aiParamDriverShortTrack",
        "aiParamDriverSpeedway", "aiParamDriverSuperspeedway",
        "aiParamPitcrewConsistency", "aiParamPitcrewSpeed",
        "aiParamPitcrewStrategy", "aiParamVehicleAero",
        "aiParamVehicleChassis", "aiParamVehicleEngine",
        "aiParamVehicleReliability",
    }};
    static constexpr std::array<std::string_view, 7> stats_keys = {{
        "NumChampionships", "NumDNF", "NumStarts", "NumTop10",
        "NumTop5", "NumWins", "Winnings",
    }};
    static constexpr std::array<std::string_view, 9> driver_keys = {{
        "birth_date", "car_class", "car_make", "car_number",
        "first_name", "home_town", "last_name", "sponsor", "team_name",
    }};
    static constexpr std::array<std::string_view, 5> section_order = {{
        "AIParamDeviation", "AIParamMean", "CareerStats",
        "CurrentYearStats", "Driver",
    }};

    std::string cini = "\r\n";
    std::set<std::string> emitted_sections;
    auto emit_section = [&](std::string_view name, const IniSection& section,
                            std::span<const std::string_view> order,
                            bool first) {
        if (!first) cini += "\r\n";
        cini += "[" + std::string(name) + "]\r\n";
        std::set<std::string> emitted;
        for (const auto key : order) {
            const auto found = section.find(std::string(key));
            if (found == section.end()) continue;
            cini += found->first + "=" + found->second + "\r\n";
            emitted.insert(found->first);
        }
        for (const auto& [key, value] : section) {
            if (emitted.contains(key)) continue;
            cini += key + "=" + value + "\r\n";
        }
    };

    bool first = true;
    for (const auto name : section_order) {
        const auto found = ini.find(std::string(name));
        if (found == ini.end()) continue;
        std::span<const std::string_view> keys;
        if (name == "AIParamDeviation" || name == "AIParamMean") keys = ai_keys;
        else if (name == "CareerStats" || name == "CurrentYearStats") keys = stats_keys;
        else keys = driver_keys;
        emit_section(name, found->second, keys, first);
        first = false;
        emitted_sections.insert(found->first);
    }
    for (const auto& [name, section] : ini) {
        if (emitted_sections.contains(name)) continue;
        emit_section(name, section, {}, first);
        first = false;
    }

    std::vector<std::uint8_t> bytes(12, 0);
    auto write_u32 = [](std::vector<std::uint8_t>& out, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            out.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    auto patch_u32 = [](std::vector<std::uint8_t>& out, std::size_t offset,
                        std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            out[offset + shift / 8] = static_cast<std::uint8_t>(value >> shift);
    };
    auto write_tag = [](std::vector<std::uint8_t>& out, const char* tag) {
        for (int index = 3; index >= 0; --index)
            out.push_back(static_cast<std::uint8_t>(tag[index]));
    };
    auto write_chunk = [&](const char* tag, std::span<const std::uint8_t> body) {
        write_tag(bytes, tag);
        write_u32(bytes, 0);
        write_u32(bytes, static_cast<std::uint32_t>(body.size()));
        bytes.insert(bytes.end(), body.begin(), body.end());
        while ((bytes.size() & 3u) != 0) bytes.push_back(0x20);
    };

    bytes[0] = 'F'; bytes[1] = 'R'; bytes[2] = 'A'; bytes[3] = 'C';
    std::array<std::uint8_t, 4> ctyp_body{{
        static_cast<std::uint8_t>(ctyp),
        static_cast<std::uint8_t>(ctyp >> 8),
        static_cast<std::uint8_t>(ctyp >> 16),
        static_cast<std::uint8_t>(ctyp >> 24),
    }};
    write_chunk("CTYP", ctyp_body);
    write_chunk("CINI", std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(cini.data()), cini.size()));
    if (!cbio_bytes.empty()) write_chunk("CBIO", cbio_bytes);
    if (!phot_bytes.empty()) write_chunk("PHOT", phot_bytes);
    if (!ctex_bytes.empty()) write_chunk("CTEX", ctex_bytes);
    if (!crew_bytes.empty()) write_chunk("CREW", crew_bytes);
    patch_u32(bytes, 8, static_cast<std::uint32_t>(bytes.size() - 12));
    return bytes;
}

namespace {
const std::string& driver_get(const std::map<std::string, IniSection>& ini, const char* key) {
    static const std::string empty;
    auto sit = ini.find("Driver");
    if (sit == ini.end()) return empty;
    auto it = sit->second.find(key);
    if (it == sit->second.end()) return empty;
    return it->second;
}
}

std::string CarFile::driver_first_name() const { return driver_get(ini, "first_name"); }
std::string CarFile::driver_last_name()  const { return driver_get(ini, "last_name");  }
std::string CarFile::team_name()         const { return driver_get(ini, "team_name");  }
std::string CarFile::sponsor()           const { return driver_get(ini, "sponsor");    }
std::string CarFile::car_number()        const { return driver_get(ini, "car_number"); }

std::string CarFile::driver_full_name() const {
    std::string a = driver_first_name();
    std::string b = driver_last_name();
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + " " + b;
}

int CarFile::car_make() const {
    const std::string& s = driver_get(ini, "car_make");
    if (s.empty()) return -1;
    try { return std::stoi(s); } catch (...) { return -1; }
}

int CarFile::car_class() const {
    const std::string& s = driver_get(ini, "car_class");
    if (s.empty()) return -1;
    try { return std::stoi(s); } catch (...) { return -1; }
}

const char* car_make_asset_name(int car_make) {
    switch (car_make) {
        case 0: return "chevrolet";
        case 1: return "dodge";
        case 2: return "ford";
        case 3: return "pontiac";
        default: return nullptr;
    }
}

}  // namespace opennr
