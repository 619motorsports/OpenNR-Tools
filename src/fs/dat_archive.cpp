#include "dat_archive.h"

#include "core/byte_reader.h"
#include "fs/dcl_blast.h"

#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace opennr {

DatArchive DatArchive::load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("DatArchive: cannot open " + path.string());
    }
    auto size = static_cast<std::streamsize>(file.tellg());
    file.seekg(0);

    DatArchive arc;
    arc.data_.resize(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(arc.data_.data()), size)) {
        throw std::runtime_error("DatArchive: short read on " + path.string());
    }
    arc.parse();
    return arc;
}

void DatArchive::parse() {
    ByteReader r(std::span<const std::uint8_t>(data_.data(), data_.size()));
    if (data_.size() < 10) {
        throw std::runtime_error("DatArchive: file too small for header");
    }
    header_hash_ = r.read_u32_le();
    // Six reserved zero bytes immediately follow the hash. We don't enforce
    // this strictly - one stray byte shouldn't break extraction.
    r.skip(6);

    // The TOC ends at the smallest data_offset we encounter. We discover that
    // bound while walking the table.
    std::size_t toc_end = data_.size();

    while (r.position() < toc_end) {
        if (r.remaining() < 15) break;

        std::size_t entry_start = r.position();

        std::uint16_t flags  = r.read_u16_le();
        std::uint32_t usize  = r.read_u32_le();
        std::uint32_t csize  = r.read_u32_le();
        std::uint32_t doff   = r.read_u32_le();
        std::uint8_t  nlen   = r.read_u8();

        // Quick sanity: a name length of zero or far too large means we
        // walked past the end of the TOC. Same for an absurd offset/size.
        bool plausible =
            nlen > 0 && nlen <= 200 &&
            (flags & 0xF000) == 0 &&
            doff >= 10 && doff <= data_.size() &&
            csize <= data_.size();

        if (!plausible) {
            // Roll back: this isn't a valid TOC entry.
            r.seek(entry_start);
            break;
        }

        // Read the name + trailing NUL.
        if (r.remaining() < static_cast<std::size_t>(nlen) + 1) break;
        std::string name(reinterpret_cast<const char*>(&data_[r.position()]), nlen);
        r.skip(nlen);
        std::uint8_t nul = r.read_u8();
        if (nul != 0) {
            r.seek(entry_start);
            break;
        }

        DatEntry entry;
        entry.flags = flags;
        entry.uncompressed_size = usize;
        entry.compressed_size   = csize;
        entry.data_offset       = doff;
        entry.name              = std::move(name);

        if (doff < toc_end) {
            toc_end = doff;
        }
        entries_.push_back(std::move(entry));
    }
}

std::span<const std::uint8_t> DatArchive::raw_bytes(const DatEntry& entry) const {
    if (entry.data_offset + entry.compressed_size > data_.size()) {
        throw std::runtime_error("DatArchive: entry out of range: " + entry.name);
    }
    return std::span<const std::uint8_t>(data_.data() + entry.data_offset,
                                          entry.compressed_size);
}

std::vector<std::uint8_t> DatArchive::read(const DatEntry& entry) const {
    auto raw = raw_bytes(entry);
    if (entry.is_compressed()) {
        auto out = dcl_decompress(raw);
        if (out.size() != entry.uncompressed_size) {
            throw std::runtime_error(
                "DatArchive: " + entry.name +
                ": decompressed size mismatch");
        }
        return out;
    }
    return std::vector<std::uint8_t>(raw.begin(), raw.end());
}

}  // namespace opennr
