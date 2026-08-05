#include "mip_texture.h"

#include "core/byte_reader.h"
#include "fs/dcl_blast.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace opennr {

namespace {

// FourCCs in .mip files are byte-reversed on disk. Same convention as
// .stp / .sim / .cam. So "BMAP" is on-disk bytes 'P','A','M','B'.
constexpr std::array<char, 4> on_disk_tag(const char (&logical)[5]) {
    return {logical[3], logical[2], logical[1], logical[0]};
}

const auto kOnDiskBMAP = on_disk_tag("BMAP");
const auto kOnDiskBMHD = on_disk_tag("BMHD");
const auto kOnDiskDATA = on_disk_tag("DATA");

bool tag_equals(std::span<const std::uint8_t> bytes, std::size_t pos,
                const std::array<char, 4>& tag) {
    if (pos + 4 > bytes.size()) return false;
    return bytes[pos    ] == static_cast<std::uint8_t>(tag[0]) &&
           bytes[pos + 1] == static_cast<std::uint8_t>(tag[1]) &&
           bytes[pos + 2] == static_cast<std::uint8_t>(tag[2]) &&
           bytes[pos + 3] == static_cast<std::uint8_t>(tag[3]);
}

}  // namespace

MipTexture MipTexture::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 32) {
        throw std::runtime_error("MipTexture: file shorter than header");
    }
    ByteReader r(bytes);

    MipTexture mip;
    mip.sig0          = r.read_u8();
    mip.sig1          = r.read_u8();
    mip.format        = r.read_u8();
    mip.header_byte_3 = r.read_u8();
    mip.field4        = r.read_u32_le();
    mip.field8        = r.read_u32_le();
    mip.color_key     = r.read_u32_le();
    mip.mip_count     = r.read_u32_le();
    mip.field20       = r.read_u32_le();
    mip.factor        = r.read_f32_le();
    mip.field28       = r.read_u32_le();

    // Walk BMAP chunks until we run out of file or hit a non-BMAP tag.
    // Trust the chunk stream rather than mip_count so malformed/truncated
    // community assets fail at their actual PIFF boundary.
    while (r.remaining() >= 12) {
        if (!tag_equals(bytes, r.position(), kOnDiskBMAP)) {
            break;
        }
        r.skip(4);
        r.read_u32_le();                          // reserved
        std::uint32_t bmap_size = r.read_u32_le();
        std::size_t bmap_end = r.position() + bmap_size;
        if (bmap_end > bytes.size()) {
            throw std::runtime_error("MipTexture: BMAP body overruns file");
        }

        // BMHD sub-chunk.
        if (!tag_equals(bytes, r.position(), kOnDiskBMHD)) {
            throw std::runtime_error("MipTexture: missing BMHD inside BMAP");
        }
        r.skip(4);
        r.read_u32_le();                          // reserved
        std::uint32_t bmhd_size = r.read_u32_le();
        if (bmhd_size < 14) {
            throw std::runtime_error("MipTexture: BMHD body too small");
        }
        auto bmhd_view = r.read_bytes(bmhd_size);

        MipLevel level;
        level.format = bmhd_view[0];
        std::memcpy(&level.width,  &bmhd_view[1],  4);
        std::memcpy(&level.height, &bmhd_view[5],  4);
        std::memcpy(&level.pitch,  &bmhd_view[9],  4);
        level.n_mips = bmhd_view[13];

        // Optional padding bytes between BMHD and DATA (observed 2 bytes
        // 0x20 0x20 in shipped files).
        while (r.position() < bmap_end &&
               !tag_equals(bytes, r.position(), kOnDiskDATA)) {
            r.skip(1);
        }
        if (!tag_equals(bytes, r.position(), kOnDiskDATA)) {
            throw std::runtime_error("MipTexture: missing DATA inside BMAP");
        }
        r.skip(4);
        r.read_u32_le();                          // reserved
        std::uint32_t payload_size = r.read_u32_le();
        if (r.position() + payload_size > bmap_end) {
            throw std::runtime_error("MipTexture: DATA payload overruns BMAP");
        }
        auto payload = r.read_bytes(payload_size);

        const std::size_t expected =
            static_cast<std::size_t>(level.pitch) *
            static_cast<std::size_t>(level.height);

        if (level.n_mips == 0) {
            // Raw pixels. Many small mip levels are stored uncompressed
            // because compressing a 1x1 / 2x2 image makes it bigger. For
            // texture-compressed formats (DXT1/DXT3) the on-disk byte
            // count is smaller than pitch*height, so we skip that check.
            const bool is_compressed_format =
                level.format == kMipFormatDXT1 ||
                level.format == kMipFormatDXT3;
            if (!is_compressed_format && payload.size() != expected) {
                throw std::runtime_error(
                    "MipTexture: raw payload size doesn't match pitch*height");
            }
            level.pixels.assign(payload.begin(), payload.end());
        } else {
            level.pixels = dcl_decompress(payload);
            const bool is_compressed_format =
                level.format == kMipFormatDXT1 ||
                level.format == kMipFormatDXT3;
            if (!is_compressed_format && level.pixels.size() != expected) {
                throw std::runtime_error(
                    "MipTexture: decompressed payload size != pitch*height");
            }
        }

        // Move past any trailing alignment padding.
        if (r.position() < bmap_end) r.seek(bmap_end);

        mip.levels.push_back(std::move(level));
    }

    return mip;
}

namespace {

bool is_power_of_two(std::uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

std::uint32_t log2_power_of_two(std::uint32_t value) {
    return static_cast<std::uint32_t>(std::countr_zero(value));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_tag(std::vector<std::uint8_t>& out, const char (&tag)[5]) {
    for (int index = 3; index >= 0; --index)
        out.push_back(static_cast<std::uint8_t>(tag[index]));
}

std::size_t begin_chunk(std::vector<std::uint8_t>& out,
                        const char (&tag)[5]) {
    append_tag(out, tag);
    append_u32(out, 0);
    const std::size_t size_offset = out.size();
    append_u32(out, 0);
    return size_offset;
}

void patch_u32(std::vector<std::uint8_t>& out, std::size_t offset,
               std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out[offset + shift / 8] = static_cast<std::uint8_t>(value >> shift);
}

void end_chunk(std::vector<std::uint8_t>& out, std::size_t size_offset) {
    const std::size_t size = out.size() - size_offset - 4;
    if (size > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("MipTexture: chunk too large");
    patch_u32(out, size_offset, static_cast<std::uint32_t>(size));
    while ((out.size() & 3u) != 0) out.push_back(0x20);
}

std::vector<std::uint8_t> downsample_rgba(
        std::span<const std::uint8_t> source,
        std::uint32_t source_width, std::uint32_t source_height,
        std::uint32_t target_width, std::uint32_t target_height) {
    // FUN_005eb320 performs alpha-weighted gamma-space reduction. Its
    // process-global table is pow(byte, 2.2); output applies the reciprocal
    // exponent after dividing by accumulated unit alpha.
    static const std::array<float, 256> gamma_table = [] {
        std::array<float, 256> table{};
        for (std::size_t index = 0; index < table.size(); ++index)
            table[index] = std::pow(float(index), 2.200000047683716f);
        return table;
    }();
    constexpr float kByteToUnit = 0.003921568859368563f;
    constexpr float kAlphaEpsilon = 9.999999747378752e-05f;
    constexpr float kInverseGamma = 1.0f / 2.200000047683716f;
    std::vector<std::uint8_t> out(
        static_cast<std::size_t>(target_width) * target_height * 4);
    for (std::uint32_t y = 0; y < target_height; ++y) {
        const std::uint32_t y0 = y * source_height / target_height;
        const std::uint32_t y1 = (y + 1) * source_height / target_height;
        for (std::uint32_t x = 0; x < target_width; ++x) {
            const std::uint32_t x0 = x * source_width / target_width;
            const std::uint32_t x1 = (x + 1) * source_width / target_width;
            std::uint32_t alpha_integer_sum = 0;
            float alpha_sum = 0.0f;
            float weighted[3]{};
            std::uint32_t count = 0;
            for (std::uint32_t sy = y0; sy < y1; ++sy) {
                for (std::uint32_t sx = x0; sx < x1; ++sx) {
                    const std::size_t offset =
                        (static_cast<std::size_t>(sy) * source_width + sx) * 4;
                    const std::uint32_t alpha = source[offset + 3];
                    alpha_integer_sum += alpha;
                    const float unit_alpha = float(alpha) * kByteToUnit;
                    alpha_sum += unit_alpha;
                    for (int channel = 0; channel < 3; ++channel)
                        weighted[channel] += unit_alpha *
                            gamma_table[source[offset + channel]];
                    ++count;
                }
            }
            const std::size_t destination =
                (static_cast<std::size_t>(y) * target_width + x) * 4;
            if (alpha_sum >= kAlphaEpsilon) {
                for (int channel = 0; channel < 3; ++channel)
                    out[destination + channel] = static_cast<std::uint8_t>(
                        std::clamp(std::nearbyint(std::pow(
                            weighted[channel] / alpha_sum, kInverseGamma)),
                            0.0f, 255.0f));
            }
            out[destination + 3] = static_cast<std::uint8_t>(
                (alpha_integer_sum + count / 2) / count);
        }
    }
    return out;
}

MipLevel encode_rgba_level(std::uint32_t width, std::uint32_t height,
                           std::span<const std::uint8_t> rgba,
                           bool opaque) {
    MipLevel level;
    level.format = opaque ? kMipFormatRGB888 : kMipFormatBGRA8888;
    level.width = width;
    level.height = height;
    level.pitch = opaque ? ((width * 3u + 3u) & ~3u) : width * 4u;
    level.n_mips = 0;
    level.pixels.assign(static_cast<std::size_t>(level.pitch) * height, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t source =
                (static_cast<std::size_t>(y) * width + x) * 4;
            const std::size_t target =
                static_cast<std::size_t>(y) * level.pitch +
                x * (opaque ? 3u : 4u);
            if (opaque) {
                level.pixels[target + 0] = rgba[source + 0];
                level.pixels[target + 1] = rgba[source + 1];
                level.pixels[target + 2] = rgba[source + 2];
            } else {
                level.pixels[target + 0] = rgba[source + 2];
                level.pixels[target + 1] = rgba[source + 1];
                level.pixels[target + 2] = rgba[source + 0];
                level.pixels[target + 3] = rgba[source + 3];
            }
        }
    }
    return level;
}

}  // namespace

MipTexture MipTexture::from_rgba8888(
        std::uint32_t width, std::uint32_t height,
        std::span<const std::uint8_t> rgba) {
    if (!is_power_of_two(width) || !is_power_of_two(height) ||
        width > 4096 || height > 4096 ||
        rgba.size() != static_cast<std::size_t>(width) * height * 4) {
        throw std::runtime_error("MipTexture: imported dimensions must be power-of-two 1..4096");
    }
    bool every_alpha_opaque = true;
    for (std::size_t index = 3; index < rgba.size(); index += 4) {
        if (rgba[index] != 0xff) { every_alpha_opaque = false; break; }
    }
    MipTexture texture;
    texture.sig0 = 4;
    texture.sig1 = 0;
    texture.format = every_alpha_opaque ? kMipFormatRGB888
                                        : kMipFormatBGRA8888;
    texture.header_byte_3 = 3;
    texture.field4 = log2_power_of_two(width);
    texture.field8 = log2_power_of_two(height);
    texture.field20 = 0;
    texture.factor = 0.8f;
    texture.field28 = 0;

    std::vector<MipLevel> largest_first;
    std::vector<std::uint8_t> current(rgba.begin(), rgba.end());
    std::uint32_t current_width = width;
    std::uint32_t current_height = height;
    for (;;) {
        largest_first.push_back(encode_rgba_level(
            current_width, current_height, current, every_alpha_opaque));
        if (current_width == 1 && current_height == 1) break;
        const std::uint32_t next_width = std::max(1u, current_width / 2u);
        const std::uint32_t next_height = std::max(1u, current_height / 2u);
        current = downsample_rgba(current, current_width, current_height,
                                  next_width, next_height);
        current_width = next_width;
        current_height = next_height;
    }
    texture.mip_count = static_cast<std::uint32_t>(largest_first.size());
    texture.levels.assign(largest_first.rbegin(), largest_first.rend());
    const auto smallest = mip_decode_to_rgba8888(texture.levels.front());
    texture.color_key =
        (static_cast<std::uint32_t>(smallest[3]) << 24) |
        (static_cast<std::uint32_t>(smallest[0]) << 16) |
        (static_cast<std::uint32_t>(smallest[1]) << 8) |
        static_cast<std::uint32_t>(smallest[2]);
    return texture;
}

std::vector<std::uint8_t> MipTexture::serialize(bool compress_data) const {
    if (levels.empty() || levels.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("MipTexture: no levels to serialize");
    std::vector<std::uint8_t> out;
    out.reserve(32);
    out.push_back(sig0);
    out.push_back(sig1);
    out.push_back(format);
    out.push_back(header_byte_3);
    append_u32(out, field4);
    append_u32(out, field8);
    append_u32(out, color_key);
    append_u32(out, static_cast<std::uint32_t>(levels.size()));
    append_u32(out, field20);
    append_u32(out, std::bit_cast<std::uint32_t>(factor));
    append_u32(out, field28);

    for (const auto& level : levels) {
        const std::size_t expected =
            static_cast<std::size_t>(level.pitch) * level.height;
        if (level.width == 0 || level.height == 0 || level.pixels.size() != expected)
            throw std::runtime_error("MipTexture: invalid raw level");
        const auto bmap = begin_chunk(out, "BMAP");
        const auto bmhd = begin_chunk(out, "BMHD");
        out.push_back(level.format);
        append_u32(out, level.width);
        append_u32(out, level.height);
        append_u32(out, level.pitch);
        std::vector<std::uint8_t> compressed;
        if (compress_data) compressed = dcl_compress(level.pixels);
        const bool use_compressed =
            !compressed.empty() && compressed.size() < level.pixels.size() / 2;
        out.push_back(use_compressed ? 1 : 0);
        end_chunk(out, bmhd);
        const auto data = begin_chunk(out, "DATA");
        if (use_compressed) {
            out.insert(out.end(), compressed.begin(), compressed.end());
        } else {
            out.insert(out.end(), level.pixels.begin(), level.pixels.end());
        }
        end_chunk(out, data);
        end_chunk(out, bmap);
    }
    return out;
}

namespace {

// Helpers for unpacking 5-bit / 6-bit channels into 8-bit values.
// Bit replication is the standard fast approximation: a 5-bit value v
// extends to 8 bits as (v << 3) | (v >> 2); a 6-bit value extends as
// (v << 2) | (v >> 4). This matches D3D's R5G6B5 / A1R5G5B5 / A4R4G4B4
// channel-extension behavior.
constexpr std::uint8_t expand5(std::uint8_t v) {
    return static_cast<std::uint8_t>((v << 3) | (v >> 2));
}
constexpr std::uint8_t expand6(std::uint8_t v) {
    return static_cast<std::uint8_t>((v << 2) | (v >> 4));
}
constexpr std::uint8_t expand4(std::uint8_t v) {
    return static_cast<std::uint8_t>((v << 4) | v);
}

inline void put_rgba(std::vector<std::uint8_t>& out, std::size_t i,
                     std::uint8_t r, std::uint8_t g,
                     std::uint8_t b, std::uint8_t a) {
    out[i + 0] = r;
    out[i + 1] = g;
    out[i + 2] = b;
    out[i + 3] = a;
}

void decode_rgb565(const MipLevel& lvl, std::vector<std::uint8_t>& out) {
    const std::uint32_t W = lvl.width, H = lvl.height, P = lvl.pitch;
    for (std::uint32_t y = 0; y < H; ++y) {
        const std::uint8_t* row = lvl.pixels.data() + y * P;
        for (std::uint32_t x = 0; x < W; ++x) {
            std::uint16_t v = static_cast<std::uint16_t>(row[2 * x]) |
                              (static_cast<std::uint16_t>(row[2 * x + 1]) << 8);
            std::uint8_t r = expand5((v >> 11) & 0x1F);
            std::uint8_t g = expand6((v >> 5)  & 0x3F);
            std::uint8_t b = expand5( v        & 0x1F);
            put_rgba(out, ((y * W) + x) * 4, r, g, b, 0xFF);
        }
    }
}

void decode_argb1555_colorkey(const MipLevel& lvl,
                              std::vector<std::uint8_t>& out,
                              std::uint32_t color_key) {
    // Type 4: 5+5+5 with one invisible color. The high bit may or may
    // not be used — observed files typically have it 0. We treat
    // transparency as "exact match against the file-level color_key in
    // its on-disk RGB555 packing".
    const std::uint16_t key16 = static_cast<std::uint16_t>(color_key & 0xFFFF);
    const std::uint32_t W = lvl.width, H = lvl.height, P = lvl.pitch;
    for (std::uint32_t y = 0; y < H; ++y) {
        const std::uint8_t* row = lvl.pixels.data() + y * P;
        for (std::uint32_t x = 0; x < W; ++x) {
            std::uint16_t v = static_cast<std::uint16_t>(row[2 * x]) |
                              (static_cast<std::uint16_t>(row[2 * x + 1]) << 8);
            std::uint8_t r = expand5((v >> 10) & 0x1F);
            std::uint8_t g = expand5((v >> 5)  & 0x1F);
            std::uint8_t b = expand5( v        & 0x1F);
            std::uint8_t a = (v == key16) ? 0 : 0xFF;
            put_rgba(out, ((y * W) + x) * 4, r, g, b, a);
        }
    }
}

void decode_argb4444(const MipLevel& lvl, std::vector<std::uint8_t>& out) {
    const std::uint32_t W = lvl.width, H = lvl.height, P = lvl.pitch;
    for (std::uint32_t y = 0; y < H; ++y) {
        const std::uint8_t* row = lvl.pixels.data() + y * P;
        for (std::uint32_t x = 0; x < W; ++x) {
            std::uint16_t v = static_cast<std::uint16_t>(row[2 * x]) |
                              (static_cast<std::uint16_t>(row[2 * x + 1]) << 8);
            std::uint8_t a = expand4((v >> 12) & 0x0F);
            std::uint8_t r = expand4((v >> 8)  & 0x0F);
            std::uint8_t g = expand4((v >> 4)  & 0x0F);
            std::uint8_t b = expand4( v        & 0x0F);
            put_rgba(out, ((y * W) + x) * 4, r, g, b, a);
        }
    }
}

void decode_rgb888(const MipLevel& lvl, std::vector<std::uint8_t>& out) {
    // Format byte 0x06 ("RGB888"): 3 bytes per pixel, **R-G-B in
    // on-disk memory order** — matching the literal channel order
    // the format name spells out.  This diverges from D3D8's
    // `D3DFMT_R8G8B8` (which stores B-G-R in memory despite the
    // name), but matches every shipped 0x06 stamp in the NR2003
    // install — verified by comparing the rendered mainmenu
    // background (`bkgrnds/main.stp`) against in-game screenshots:
    // assuming BGR-on-disk produces a blue-tinted sunset and blue
    // cars, while RGB-on-disk produces the correct red-tinted
    // sunset and the NASCAR cars' authored paint schemes.
    const std::uint32_t W = lvl.width, H = lvl.height, P = lvl.pitch;
    for (std::uint32_t y = 0; y < H; ++y) {
        const std::uint8_t* row = lvl.pixels.data() + y * P;
        for (std::uint32_t x = 0; x < W; ++x) {
            std::uint8_t r = row[3 * x + 0];
            std::uint8_t g = row[3 * x + 1];
            std::uint8_t b = row[3 * x + 2];
            put_rgba(out, ((y * W) + x) * 4, r, g, b, 0xFF);
        }
    }
}

void decode_bgra8888(const MipLevel& lvl, std::vector<std::uint8_t>& out) {
    // D3DFMT_A8R8G8B8 stores B-G-R-A in memory order on little-endian.
    const std::uint32_t W = lvl.width, H = lvl.height, P = lvl.pitch;
    for (std::uint32_t y = 0; y < H; ++y) {
        const std::uint8_t* row = lvl.pixels.data() + y * P;
        for (std::uint32_t x = 0; x < W; ++x) {
            std::uint8_t b = row[4 * x + 0];
            std::uint8_t g = row[4 * x + 1];
            std::uint8_t r = row[4 * x + 2];
            std::uint8_t a = row[4 * x + 3];
            put_rgba(out, ((y * W) + x) * 4, r, g, b, a);
        }
    }
}

// Decode the 8-byte DXT1 color sub-block into a 4x4 RGBA8888 patch.
// `out_block` must have 16*4 = 64 bytes.  When `dxt1_alpha` is true and
// color0 <= color1, code-3 produces transparent black (punch-through
// alpha).  When false (DXT3 carries explicit alpha) code-3 is opaque
// black and the punch-through rule does not apply.
inline void decode_dxt_color_block(const std::uint8_t* src,
                                   std::uint8_t* out_block,
                                   bool dxt1_alpha) {
    std::uint16_t c0 = static_cast<std::uint16_t>(src[0]) |
                       (static_cast<std::uint16_t>(src[1]) << 8);
    std::uint16_t c1 = static_cast<std::uint16_t>(src[2]) |
                       (static_cast<std::uint16_t>(src[3]) << 8);

    std::uint8_t r0 = expand5((c0 >> 11) & 0x1F);
    std::uint8_t g0 = expand6((c0 >>  5) & 0x3F);
    std::uint8_t b0 = expand5( c0        & 0x1F);
    std::uint8_t r1 = expand5((c1 >> 11) & 0x1F);
    std::uint8_t g1 = expand6((c1 >>  5) & 0x3F);
    std::uint8_t b1 = expand5( c1        & 0x1F);

    std::uint8_t palette[4][4];
    palette[0][0] = r0; palette[0][1] = g0; palette[0][2] = b0; palette[0][3] = 0xFF;
    palette[1][0] = r1; palette[1][1] = g1; palette[1][2] = b1; palette[1][3] = 0xFF;
    if (c0 > c1 || !dxt1_alpha) {
        // 4-colour block: linear blend in 1:2 and 2:1 ratios.
        palette[2][0] = static_cast<std::uint8_t>((2 * r0 + r1 + 1) / 3);
        palette[2][1] = static_cast<std::uint8_t>((2 * g0 + g1 + 1) / 3);
        palette[2][2] = static_cast<std::uint8_t>((2 * b0 + b1 + 1) / 3);
        palette[2][3] = 0xFF;
        palette[3][0] = static_cast<std::uint8_t>((r0 + 2 * r1 + 1) / 3);
        palette[3][1] = static_cast<std::uint8_t>((g0 + 2 * g1 + 1) / 3);
        palette[3][2] = static_cast<std::uint8_t>((b0 + 2 * b1 + 1) / 3);
        palette[3][3] = 0xFF;
    } else {
        // 3-colour + transparent block.  Code 2 is the midpoint; code
        // 3 is RGB=0, A=0.  Only applies to DXT1.
        palette[2][0] = static_cast<std::uint8_t>((r0 + r1) / 2);
        palette[2][1] = static_cast<std::uint8_t>((g0 + g1) / 2);
        palette[2][2] = static_cast<std::uint8_t>((b0 + b1) / 2);
        palette[2][3] = 0xFF;
        palette[3][0] = 0;
        palette[3][1] = 0;
        palette[3][2] = 0;
        palette[3][3] = 0;
    }

    std::uint32_t indices =
        static_cast<std::uint32_t>(src[4])        |
        (static_cast<std::uint32_t>(src[5]) <<  8) |
        (static_cast<std::uint32_t>(src[6]) << 16) |
        (static_cast<std::uint32_t>(src[7]) << 24);
    for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
            std::uint8_t code = (indices >> (2 * (py * 4 + px))) & 0x3;
            int o = (py * 4 + px) * 4;
            out_block[o + 0] = palette[code][0];
            out_block[o + 1] = palette[code][1];
            out_block[o + 2] = palette[code][2];
            out_block[o + 3] = palette[code][3];
        }
    }
}

void decode_dxt1(const MipLevel& lvl, std::vector<std::uint8_t>& out) {
    const std::uint32_t W = lvl.width, H = lvl.height;
    const std::uint32_t blocks_x = (W + 3) / 4;
    const std::uint32_t blocks_y = (H + 3) / 4;
    const std::uint8_t* src = lvl.pixels.data();
    std::uint8_t patch[64];
    for (std::uint32_t by = 0; by < blocks_y; ++by) {
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            decode_dxt_color_block(src, patch, /*dxt1_alpha=*/true);
            src += 8;
            for (int py = 0; py < 4; ++py) {
                std::uint32_t y = by * 4 + py;
                if (y >= H) break;
                for (int px = 0; px < 4; ++px) {
                    std::uint32_t x = bx * 4 + px;
                    if (x >= W) break;
                    std::size_t o = ((y * W) + x) * 4;
                    int p = (py * 4 + px) * 4;
                    out[o + 0] = patch[p + 0];
                    out[o + 1] = patch[p + 1];
                    out[o + 2] = patch[p + 2];
                    out[o + 3] = patch[p + 3];
                }
            }
        }
    }
}

void decode_dxt3(const MipLevel& lvl, std::vector<std::uint8_t>& out) {
    const std::uint32_t W = lvl.width, H = lvl.height;
    const std::uint32_t blocks_x = (W + 3) / 4;
    const std::uint32_t blocks_y = (H + 3) / 4;
    const std::uint8_t* src = lvl.pixels.data();
    std::uint8_t patch[64];
    for (std::uint32_t by = 0; by < blocks_y; ++by) {
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            // Eight bytes of explicit 4-bit alpha (16 nibbles, row-major
            // within the 4x4 block, low nibble first).
            std::uint8_t alpha_block[16];
            for (int i = 0; i < 8; ++i) {
                std::uint8_t lo = src[i] & 0x0F;
                std::uint8_t hi = (src[i] >> 4) & 0x0F;
                alpha_block[i * 2 + 0] = expand4(lo);
                alpha_block[i * 2 + 1] = expand4(hi);
            }
            // The colour block is the same as DXT1, but the
            // punch-through-alpha rule does not apply: always treat as
            // a 4-colour palette.
            decode_dxt_color_block(src + 8, patch, /*dxt1_alpha=*/false);
            src += 16;
            for (int py = 0; py < 4; ++py) {
                std::uint32_t y = by * 4 + py;
                if (y >= H) break;
                for (int px = 0; px < 4; ++px) {
                    std::uint32_t x = bx * 4 + px;
                    if (x >= W) break;
                    std::size_t o = ((y * W) + x) * 4;
                    int p = (py * 4 + px) * 4;
                    out[o + 0] = patch[p + 0];
                    out[o + 1] = patch[p + 1];
                    out[o + 2] = patch[p + 2];
                    out[o + 3] = alpha_block[py * 4 + px];
                }
            }
        }
    }
}

}  // namespace

std::vector<std::uint8_t> mip_decode_to_rgba8888(const MipLevel& lvl,
                                                 std::uint32_t color_key) {
    if (lvl.width == 0 || lvl.height == 0) return {};
    std::vector<std::uint8_t> out(
        static_cast<std::size_t>(lvl.width) * lvl.height * 4);
    switch (lvl.format) {
        case kMipFormatRGB565:    decode_rgb565(lvl, out); break;
        case kMipFormatRGB555Key: decode_argb1555_colorkey(lvl, out, color_key); break;
        case kMipFormatARGB4444:  decode_argb4444(lvl, out); break;
        case kMipFormatRGB888:    decode_rgb888(lvl, out); break;
        case kMipFormatBGRA8888:  decode_bgra8888(lvl, out); break;
        case kMipFormatDXT1:      decode_dxt1(lvl, out); break;
        case kMipFormatDXT3:      decode_dxt3(lvl, out); break;
        default:
            throw std::runtime_error(
                "mip_decode_to_rgba8888: unknown format byte");
    }
    return out;
}

}  // namespace opennr
