#include "dcl_blast.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

#include "pklib/implode.h"

namespace opennr {

namespace {

struct CompressState {
    std::span<const std::uint8_t> input;
    std::size_t offset = 0;
    std::vector<std::uint8_t> output;
};

unsigned int dcl_read_callback(char* buffer, unsigned int* size,
                               void* opaque) {
    auto& state = *static_cast<CompressState*>(opaque);
    const auto remaining = state.input.size() - state.offset;
    const auto requested = static_cast<std::size_t>(*size);
    const auto count = std::min(remaining, requested);
    if (count != 0) {
        std::copy_n(state.input.data() + state.offset, count,
                    reinterpret_cast<std::uint8_t*>(buffer));
        state.offset += count;
    }
    *size = static_cast<unsigned int>(count);
    return static_cast<unsigned int>(count);
}

void dcl_write_callback(char* buffer, unsigned int* size, void* opaque) {
    auto& state = *static_cast<CompressState*>(opaque);
    const auto count = static_cast<std::size_t>(*size);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(buffer);
    state.output.insert(state.output.end(), bytes, bytes + count);
}

}  // namespace

namespace {

// RLE-encoded canonical bit lengths.  Each byte = (count_minus_1 << 4) | length.
constexpr std::array<std::uint8_t, 6>  kLenRle  {0x02, 0x23, 0x24, 0x35, 0x26, 0x17};
constexpr std::array<std::uint8_t, 7>  kDistRle {0x02, 0x14, 0x35, 0xE6, 0xF7, 0x97, 0xF8};
// Literal RLE: per the public DCL spec.  Only used when the stream's
// first byte == 1 (Huffman-coded literals); stock NR2003 data uses the
// uncoded path so this table is exercised mainly by mods.
constexpr std::uint8_t kLitRle[] = {
    11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
    9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
    7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12, 8,
    12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
    44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
    44, 173,
};

// Length-symbol decoding parameters.
constexpr std::array<std::uint16_t, 16> kLenBase  {3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264};
constexpr std::array<std::uint8_t,  16> kLenExtra {0, 0, 0, 0, 0, 0, 0, 0, 1,  2,  3,  4,  5,  6,  7,  8};

constexpr int kMaxBits = 13;

// One canonical-Huffman decoder with DCL's bit-inversion convention.
struct HuffmanTable {
    int          count[kMaxBits + 1] = {};
    std::vector<std::uint16_t> symbols;

    HuffmanTable(std::span<const std::uint8_t> rle, std::size_t total_symbols) {
        // Expand RLE to per-symbol bit lengths.
        std::vector<std::uint8_t> lengths;
        lengths.reserve(total_symbols);
        for (std::uint8_t b : rle) {
            std::uint8_t length = b & 0x0F;
            int n = (b >> 4) + 1;
            for (int i = 0; i < n; ++i) lengths.push_back(length);
        }
        if (lengths.size() != total_symbols) {
            throw std::runtime_error("DCL: RLE blob length mismatch");
        }
        // Bucket counts.
        for (auto l : lengths) {
            if (l > kMaxBits) throw std::runtime_error("DCL: bit length out of range");
            if (l != 0) count[l]++;
        }
        // Symbols sorted by (length, symbol_index).
        symbols.reserve(total_symbols);
        for (int k = 1; k <= kMaxBits; ++k) {
            for (std::size_t s = 0; s < lengths.size(); ++s) {
                if (lengths[s] == k) symbols.push_back(static_cast<std::uint16_t>(s));
            }
        }
    }
};

// LSB-first bit stream over a borrowed byte span.
class BitReader {
public:
    BitReader(std::span<const std::uint8_t> data, std::size_t start)
        : data_(data), pos_(start), buf_(0), nbits_(0) {}

    std::uint32_t read_bits(int n) {
        while (nbits_ < n) {
            if (pos_ >= data_.size()) {
                throw std::runtime_error("DCL: stream truncated");
            }
            buf_ |= static_cast<std::uint32_t>(data_[pos_++]) << nbits_;
            nbits_ += 8;
        }
        std::uint32_t value = buf_ & ((1u << n) - 1u);
        buf_  >>= n;
        nbits_ -= n;
        return value;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t   pos_;
    std::uint32_t buf_;
    int           nbits_;
};

int decode_huffman(BitReader& br, const HuffmanTable& t) {
    int code  = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        int bit = static_cast<int>(br.read_bits(1)) ^ 1;
        code = (code << 1) | bit;
        int cnt = t.count[len];
        if (code < first + cnt) {
            return t.symbols[index + (code - first)];
        }
        index += cnt;
        first = (first + cnt) << 1;
    }
    throw std::runtime_error("DCL: Huffman code did not match any symbol");
}

}  // namespace

std::vector<std::uint8_t> dcl_decompress(std::span<const std::uint8_t> input) {
    if (input.size() < 2) {
        throw std::runtime_error("DCL: stream shorter than 2-byte header");
    }
    int literal_type = input[0];
    int dict_log2    = input[1];
    if (literal_type != 0 && literal_type != 1) {
        throw std::runtime_error("DCL: bad literal type");
    }
    if (dict_log2 < 4 || dict_log2 > 6) {
        throw std::runtime_error("DCL: bad dictionary size");
    }

    HuffmanTable len_table (std::span<const std::uint8_t>(kLenRle.data(), kLenRle.size()), 16);
    HuffmanTable dist_table(std::span<const std::uint8_t>(kDistRle.data(), kDistRle.size()), 64);
    // Build literal table lazily: NR2003 stock data uses uncoded literals,
    // and the table allocates a 256-entry symbol vector.
    std::optional<HuffmanTable> lit_table;
    if (literal_type == 1) {
        lit_table.emplace(std::span<const std::uint8_t>(kLitRle, sizeof(kLitRle)), 256);
    }

    BitReader br(input, 2);
    std::vector<std::uint8_t> out;
    out.reserve(input.size() * 4);

    while (true) {
        int flag = static_cast<int>(br.read_bits(1));
        if (flag) {
            // Length+distance back-reference.
            int len_sym = decode_huffman(br, len_table);
            int length  = kLenBase[len_sym];
            int extra   = kLenExtra[len_sym];
            if (extra) {
                length += static_cast<int>(br.read_bits(extra));
            }
            if (length == 519) break;  // end-of-stream

            int extra_bits = (length == 2) ? 2 : dict_log2;
            int dist_sym   = decode_huffman(br, dist_table);
            int distance   = (dist_sym << extra_bits)
                           | static_cast<int>(br.read_bits(extra_bits));
            distance += 1;
            if (static_cast<std::size_t>(distance) > out.size()) {
                throw std::runtime_error("DCL: backref distance exceeds output");
            }
            std::size_t src = out.size() - static_cast<std::size_t>(distance);
            for (int i = 0; i < length; ++i) {
                out.push_back(out[src++]);
            }
        } else {
            // Literal byte.
            if (literal_type == 0) {
                out.push_back(static_cast<std::uint8_t>(br.read_bits(8)));
            } else {
                int sym = decode_huffman(br, *lit_table);
                out.push_back(static_cast<std::uint8_t>(sym));
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> dcl_compress(
        std::span<const std::uint8_t> input) {
    if (input.size() > std::numeric_limits<unsigned int>::max())
        throw std::runtime_error("DCL input is too large");
    CompressState state{input};
    std::vector<char> work(CMP_BUFFER_SIZE);
    // PKLIB's FindRep walks the pair-offset table until it reaches an offset
    // at or beyond the current input window.  SortBuffer only writes entries
    // for hashes that occur, so an unused tail must be an out-of-window
    // sentinel rather than zero (which points back at work_buff[0]).
    auto* cmp = reinterpret_cast<TCmpStruct*>(work.data());
    std::fill(std::begin(cmp->phash_offs), std::end(cmp->phash_offs),
              static_cast<unsigned short>(0xffff));
    unsigned int type = CMP_BINARY;
    unsigned int dictionary = CMP_IMPLODE_DICT_SIZE3;
    const auto result = implode(
        dcl_read_callback, dcl_write_callback, work.data(), &state,
        &type, &dictionary);
    if (result != CMP_NO_ERROR)
        throw std::runtime_error("DCL compression failed");
    return state.output;
}

}  // namespace opennr
