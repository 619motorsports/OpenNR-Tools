#pragma once

// Clean-room reimplementation of Papyrus rts.dll's PIFF chunk container.
// PIFF = "Papyrus Interchange File Format" — the IFF-style outer
// framing used by .3do, .ptf, .dat and other Papyrus assets.
//
// Chunk header layout (confirmed against rts.dll piffReadChunkHeader
// @ 0x1000d530 and piffWriteChunkHeader @ 0x1000d370):
//
//   u32  fourcc          // chunk type tag (often spelled in ASCII)
//   u32  version         // per-chunk version
//   i32  payload_size    // payload byte count, signed (so negatives
//                        // can mark special states — observed values
//                        // are non-negative)
//   u8[payload_size]     // chunk body
//   u8[pad]              // pad up to 4-byte boundary
//
// piffReadChunk pads the cursor with `4 - (size & 3)` extra bytes when
// `size` is not a multiple of 4, so consumers must round up to multiples
// of 4 when skipping payloads.

#include "core/byte_reader.h"

#include <cstdint>
#include <optional>
#include <span>

namespace opennr::papyrus {

struct PiffHeader {
    std::uint32_t fourcc = 0;
    std::uint32_t version = 0;
    std::int32_t  payload_size = 0;
};

// Helper: build a u32 from a 4-character ASCII tag, little-endian
// (so e.g. fourcc_of("PIFF") packs 'P' into the low byte to match
// rts.dll's `_fileReadUint32` interpretation).
constexpr std::uint32_t fourcc_of(const char (&tag)[5]) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(tag[0])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(tag[1])) << 8)  |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(tag[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(tag[3])) << 24);
}

inline std::size_t piff_payload_padded(std::int32_t size) {
    if (size < 0) return 0;
    auto u = static_cast<std::size_t>(size);
    return (u + 3u) & ~std::size_t{3u};
}

class PiffReader {
public:
    explicit PiffReader(std::span<const std::uint8_t> bytes);

    // Read the next chunk header, advancing the cursor past the 12-byte
    // header.  Returns nullopt on EOF.  Throws on truncated header.
    std::optional<PiffHeader> read_header();

    // Returns a borrowed view of the payload for the chunk whose header
    // was just read.  Advances the cursor past the payload AND the
    // 4-byte-boundary padding.
    std::span<const std::uint8_t> read_payload(const PiffHeader& h);

    // Skip the payload + padding without copying.
    void skip_payload(const PiffHeader& h);

    std::size_t position() const { return reader_.position(); }
    std::size_t remaining() const { return reader_.remaining(); }

private:
    ByteReader reader_;
    std::span<const std::uint8_t> bytes_;
};

}  // namespace opennr::papyrus
