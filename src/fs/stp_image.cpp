#include "stp_image.h"

#include "core/byte_reader.h"
#include "fs/dcl_blast.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace opennr {

namespace {

// FourCCs in .stp files are written byte-reversed on disk. So for the
// logical tag "STMP" the on-disk bytes are 'P','M','T','S'. Each chunk is
// (tag:4, reserved:u32 LE, body_size:u32 LE, body[body_size]).
constexpr std::array<char, 4> on_disk_tag(const char (&logical)[5]) {
    return {logical[3], logical[2], logical[1], logical[0]};
}

const auto kOnDiskSTMP = on_disk_tag("STMP");
const auto kOnDiskSTHD = on_disk_tag("STHD");
const auto kOnDiskDATA = on_disk_tag("DATA");
const auto kOnDiskBMAP = on_disk_tag("BMAP");
const auto kOnDiskBMHD = on_disk_tag("BMHD");

bool tag_equals(std::span<const std::uint8_t> bytes, std::size_t pos,
                const std::array<char, 4>& tag) {
    if (pos + 4 > bytes.size()) return false;
    return bytes[pos    ] == static_cast<std::uint8_t>(tag[0]) &&
           bytes[pos + 1] == static_cast<std::uint8_t>(tag[1]) &&
           bytes[pos + 2] == static_cast<std::uint8_t>(tag[2]) &&
           bytes[pos + 3] == static_cast<std::uint8_t>(tag[3]);
}

}  // namespace

StpImage StpImage::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12) {
        throw std::runtime_error("StpImage: file shorter than chunk header");
    }
    if (!tag_equals(bytes, 0, kOnDiskSTMP)) {
        throw std::runtime_error("StpImage: missing STMP magic");
    }
    ByteReader r(bytes);
    r.skip(4);                       // STMP tag
    r.read_u32_le();                 // reserved (observed 0)
    std::uint32_t outer_size = r.read_u32_le();
    if (12 + outer_size > bytes.size()) {
        throw std::runtime_error("StpImage: STMP body overruns file");
    }
    const std::size_t outer_end = 12 + outer_size;

    StpImage out;
    bool have_bmhd = false;
    bool have_payload = false;
    std::span<const std::uint8_t> bmap_data_payload;

    while (r.position() < outer_end) {
        if (r.remaining() < 12) break;
        std::size_t chunk_pos = r.position();

        if (tag_equals(bytes, chunk_pos, kOnDiskSTHD)) {
            r.skip(4);
            r.read_u32_le();
            std::uint32_t body_size = r.read_u32_le();
            if (body_size < 4) {
                throw std::runtime_error("StpImage: STHD body too small");
            }
            out.sthd_count = r.read_u32_le();
            // Skip any tail beyond the count we read.
            std::size_t consumed = 4;
            if (body_size > consumed) {
                r.skip(body_size - consumed);
            }
        } else if (tag_equals(bytes, chunk_pos, kOnDiskDATA)) {
            r.skip(4);
            r.read_u32_le();
            std::uint32_t body_size = r.read_u32_le();
            // 12 bytes per sub-image: reserved:u32, w:u16, h:u16, reserved:u32
            std::size_t end = r.position() + body_size;
            while (r.position() + 12 <= end) {
                r.read_u32_le();
                std::uint16_t w = r.read_u16_le();
                std::uint16_t h = r.read_u16_le();
                r.read_u32_le();
                out.subimages.push_back({w, h});
            }
            if (r.position() < end) r.seek(end);
        } else if (tag_equals(bytes, chunk_pos, kOnDiskBMAP)) {
            r.skip(4);
            r.read_u32_le();
            std::uint32_t body_size = r.read_u32_le();
            std::size_t bmap_end = r.position() + body_size;
            if (bmap_end > bytes.size()) {
                throw std::runtime_error("StpImage: BMAP body overruns file");
            }

            // BMHD sub-chunk.
            if (!tag_equals(bytes, r.position(), kOnDiskBMHD)) {
                throw std::runtime_error("StpImage: missing BMHD inside BMAP");
            }
            r.skip(4);
            r.read_u32_le();
            std::uint32_t bmhd_size = r.read_u32_le();
            if (bmhd_size < 14) {
                throw std::runtime_error("StpImage: BMHD body too small");
            }
            auto bmhd_view = r.read_bytes(bmhd_size);
            out.format    = bmhd_view[0];
            std::memcpy(&out.width,  &bmhd_view[1],  4);
            std::memcpy(&out.height, &bmhd_view[5],  4);
            std::memcpy(&out.pitch,  &bmhd_view[9],  4);
            out.n_mips    = bmhd_view[13];
            have_bmhd = true;

            // Optional padding bytes between BMHD and DATA (observed 2).
            while (r.position() < bmap_end &&
                   !tag_equals(bytes, r.position(), kOnDiskDATA)) {
                r.skip(1);
            }
            if (!tag_equals(bytes, r.position(), kOnDiskDATA)) {
                throw std::runtime_error("StpImage: missing DATA inside BMAP");
            }
            r.skip(4);
            r.read_u32_le();
            std::uint32_t payload_size = r.read_u32_le();
            if (r.position() + payload_size > bmap_end) {
                throw std::runtime_error("StpImage: DATA payload overruns BMAP");
            }
            bmap_data_payload = r.read_bytes(payload_size);
            have_payload = true;

            if (r.position() < bmap_end) r.seek(bmap_end);
        } else {
            // Unknown chunk; stop walking rather than guessing past it.
            break;
        }
    }

    if (!have_bmhd || !have_payload) {
        throw std::runtime_error("StpImage: BMAP image missing");
    }

    const std::size_t expected_size =
        static_cast<std::size_t>(out.pitch) * static_cast<std::size_t>(out.height);

    if (out.n_mips == 0) {
        if (bmap_data_payload.size() != expected_size) {
            throw std::runtime_error(
                "StpImage: raw payload size doesn't match pitch*height");
        }
        out.pixels.assign(bmap_data_payload.begin(), bmap_data_payload.end());
    } else {
        out.pixels = dcl_decompress(bmap_data_payload);
        if (out.pixels.size() != expected_size) {
            throw std::runtime_error(
                "StpImage: decompressed payload size != pitch*height");
        }
    }

    return out;
}

}  // namespace opennr
