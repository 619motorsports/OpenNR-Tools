#pragma once

// PKWARE DCL "implode" / "blast" decompressor.
//
// This codec is what NR2003 uses for its compressed .dat archive entries.
// The format is publicly documented (Ben Rudiak-Gould, 2001 article in
// comp.compression) and Mark Adler's blast.c reference implementation
// is permissively licensed (zlib license).  This file is a clean-room
// port of the published algorithm.
//
// Stream layout:
//
//   byte 0   literal_type      0 = uncoded 8-bit literals
//                                 1 = Huffman-coded literals
//   byte 1   dict_extra_bits   4 / 5 / 6 (extra distance bits;
//                                 dictionary size = 64 << this value)
//   ...      LSB-first bit stream of literals + length/distance pairs.

#include <cstdint>
#include <span>
#include <vector>

namespace opennr {

// Decompress one DCL/blast stream.  Throws std::runtime_error on bad input.
std::vector<std::uint8_t> dcl_decompress(std::span<const std::uint8_t> input);

// Compress one payload with the binary, 4096-byte-dictionary PKWARE DCL
// "implode" profile used by the retail RTS writer. The returned bytes include
// the two-byte DCL stream header.
std::vector<std::uint8_t> dcl_compress(std::span<const std::uint8_t> input);

}  // namespace opennr
