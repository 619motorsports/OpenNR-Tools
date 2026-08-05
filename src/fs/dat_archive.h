#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace opennr {

// One entry in a Papyrus .dat archive.
//
// See docs/formats/dat_archive.md for the on-disk layout.
struct DatEntry {
    std::uint16_t flags = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t data_offset = 0;
    std::string   name;

    static constexpr std::uint16_t kFlagCompressed = 0x0200;

    bool is_compressed() const noexcept { return (flags & kFlagCompressed) != 0; }
};

// In-memory view of a Papyrus .dat archive. Owns the file bytes.
class DatArchive {
public:
    static DatArchive load(const std::filesystem::path& path);

    const std::vector<DatEntry>& entries() const noexcept { return entries_; }

    // Returns a borrowed view of the entry's raw on-disk bytes (still
    // compressed for compressed entries).  Use this when you need the
    // original DCL stream; otherwise prefer `read`.
    std::span<const std::uint8_t> raw_bytes(const DatEntry& entry) const;

    // Returns the entry's UNCOMPRESSED bytes - decompresses on the fly for
    // compressed entries, copies for stored entries.  Throws on bad streams.
    std::vector<std::uint8_t>     read(const DatEntry& entry) const;

    std::uint32_t header_hash() const noexcept { return header_hash_; }

private:
    std::vector<std::uint8_t> data_;
    std::vector<DatEntry>     entries_;
    std::uint32_t             header_hash_ = 0;

    void parse();
};

}  // namespace opennr
