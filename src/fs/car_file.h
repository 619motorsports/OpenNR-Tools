#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace opennr {

// Parsed contents of one section of a .car CINI block.
using IniSection = std::map<std::string, std::string>;

// Parsed view of a Papyrus .car file.
//
// See docs/formats/car_file.md for the on-disk layout. Texture
// payloads (CTEX / CREW) are kept as raw byte spans here; callers can
// pass them to MipTexture::parse to walk the mipmap chunks.
struct CarFile {
    std::uint32_t ctyp = 0;

    // Parsed CINI sections, keyed by section name (case-sensitive).
    std::map<std::string, IniSection> ini;

    // Raw CTEX / CREW payload bytes (the body after the 12-byte sub-chunk
    // header). Either may be empty if absent.
    std::vector<std::uint8_t> cbio_bytes;
    std::vector<std::uint8_t> phot_bytes;
    std::vector<std::uint8_t> ctex_bytes;
    std::vector<std::uint8_t> crew_bytes;

    // Convenience accessors backed by ini["Driver"].
    std::string driver_first_name() const;
    std::string driver_last_name() const;
    std::string driver_full_name() const;
    std::string team_name() const;
    std::string sponsor() const;
    std::string car_number() const;     // raw string, possibly leading spaces
    int         car_make()    const;    // -1 if absent / unparsable
    int         car_class()   const;    // -1 if absent / unparsable

    static CarFile parse(std::span<const std::uint8_t> bytes);

    // Serialize the object using the original writer's CARF chunk order and
    // ProfileManager text contract: leading CRLF, fixed section/key order,
    // CRLF lines, and 4-byte 0x20 chunk alignment.
    std::vector<std::uint8_t> serialize() const;
};

// `[Driver] car_make` index → manufacturer asset name as used by the
// shipped art (`card_<name>.mip`, `art\logo_mfg_lg_<name>.stp`):
// 0=chevrolet, 1=dodge, 2=ford, 3=pontiac — verified against the
// shipped 2003-season .car files (Gordon/Johnson/Stewart = 0,
// Elliott/Newman/Wallace = 1, Kenseth = 2).  Returns nullptr for
// out-of-range values.
const char* car_make_asset_name(int car_make);

}  // namespace opennr
