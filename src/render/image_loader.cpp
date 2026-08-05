#include "image_loader.h"

#include "fs/tga_image.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace opennr::render {

namespace {

// Largest level of a parsed .mip is the last entry (smallest mip first).
const MipLevel* largest_level(const MipTexture& tex) {
    if (tex.levels.empty()) return nullptr;
    const MipLevel* best = &tex.levels.front();
    for (const auto& l : tex.levels) {
        if (std::size_t(l.width) * l.height >
            std::size_t(best->width) * best->height) {
            best = &l;
        }
    }
    return best;
}

}  // namespace

DecodedImage decode_stp_rgba8(const StpImage& img) {
    if (img.width == 0 || img.height == 0 || img.pixels.empty()) {
        return {};
    }
    // Build a transient MipLevel from the .stp body so we can reuse the
    // pixel-format switch in mip_texture.cpp. The two containers share
    // the format byte, pitch convention, and DCL handling.
    MipLevel lvl;
    lvl.format = img.format;
    lvl.width  = img.width;
    lvl.height = img.height;
    lvl.pitch  = img.pitch;
    lvl.n_mips = img.n_mips;
    lvl.pixels = img.pixels;

    DecodedImage out;
    out.width  = img.width;
    out.height = img.height;
    out.rgba8  = mip_decode_to_rgba8888(lvl, /*color_key=*/0);

    // Some shipped stamps have format 0x07 (BGRA) but were authored
    // with a fully-zero alpha channel as the "opaque" sentinel; the
    // runtime presumably promotes a=0 to a=255 for them.  Detect an
    // all-zero alpha buffer and promote so the image stays visible.
    bool has_nonzero_alpha = false;
    for (std::size_t i = 3; i < out.rgba8.size(); i += 4) {
        if (out.rgba8[i] != 0) { has_nonzero_alpha = true; break; }
    }
    if (!has_nonzero_alpha) {
        for (std::size_t i = 3; i < out.rgba8.size(); i += 4) {
            out.rgba8[i] = 0xff;
        }
    }
    return out;
}

DecodedImage decode_stp_rgba8(std::span<const std::uint8_t> bytes) {
    return decode_stp_rgba8(StpImage::parse(bytes));
}

DecodedImage decode_mip_rgba8(const MipTexture& tex) {
    const MipLevel* lvl = largest_level(tex);
    if (!lvl) return {};
    DecodedImage out;
    out.width  = lvl->width;
    out.height = lvl->height;
    out.rgba8  = mip_decode_to_rgba8888(*lvl, tex.color_key);
    out.address_u = tex.clamp_u() ? AddressMode::clamp : AddressMode::wrap;
    out.address_v = tex.clamp_v() ? AddressMode::clamp : AddressMode::wrap;
    return out;
}

DecodedImage decode_mip_rgba8(std::span<const std::uint8_t> bytes) {
    return decode_mip_rgba8(MipTexture::parse(bytes));
}

DecodedImage decode_tga_rgba8(std::span<const std::uint8_t> bytes) {
    const auto image = TgaImage::parse(bytes);
    return {image.width, image.height, image.rgba,
            AddressMode::clamp, AddressMode::clamp};
}

DecodedImage decode_bmp_rgba8(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 54 || bytes[0] != 'B' || bytes[1] != 'M')
        throw std::runtime_error("BMP: unsupported or truncated header");
    const auto u16 = [&bytes](std::size_t at) {
        return std::uint16_t(bytes[at]) |
               (std::uint16_t(bytes[at + 1]) << 8);
    };
    const auto u32 = [&bytes](std::size_t at) {
        return std::uint32_t(bytes[at]) |
               (std::uint32_t(bytes[at + 1]) << 8) |
               (std::uint32_t(bytes[at + 2]) << 16) |
               (std::uint32_t(bytes[at + 3]) << 24);
    };
    const auto s32 = [&u32](std::size_t at) {
        return static_cast<std::int32_t>(u32(at));
    };
    const std::uint32_t pixel_offset = u32(10);
    const std::uint32_t dib_size = u32(14);
    if (dib_size < 40 || bytes.size() < 14u + dib_size)
        throw std::runtime_error("BMP: unsupported DIB");
    const std::int32_t width = s32(18);
    const std::int32_t height = s32(22);
    if (width <= 0 || height == 0 || width > 4096 ||
        height == std::numeric_limits<std::int32_t>::min() ||
        std::abs(height) > 4096)
        throw std::runtime_error("BMP: invalid dimensions");
    if (u16(26) != 1 || (u16(28) != 24 && u16(28) != 32) ||
        u32(30) != 0)
        throw std::runtime_error("BMP: unsupported pixel format");
    const std::uint32_t w = static_cast<std::uint32_t>(width);
    const std::uint32_t h = static_cast<std::uint32_t>(
        height < 0 ? -static_cast<std::int64_t>(height) : height);
    const std::size_t bytes_per_pixel = u16(28) / 8u;
    const std::size_t row_bytes =
        ((std::size_t(w) * bytes_per_pixel + 3u) / 4u) * 4u;
    if (row_bytes > std::numeric_limits<std::size_t>::max() / h ||
        pixel_offset > bytes.size() ||
        row_bytes * h > bytes.size() - pixel_offset)
        throw std::runtime_error("BMP: truncated pixel data");

    DecodedImage out;
    out.width = w;
    out.height = h;
    out.rgba8.resize(std::size_t(w) * h * 4u);
    const bool top_down = height < 0;
    for (std::uint32_t sy = 0; sy < h; ++sy) {
        const auto dy = top_down ? sy : h - sy - 1;
        const auto* row = bytes.data() + pixel_offset + row_bytes * sy;
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto* px = row + std::size_t(x) * bytes_per_pixel;
            auto* dst = out.rgba8.data() +
                        (std::size_t(dy) * w + x) * 4u;
            dst[0] = px[2]; dst[1] = px[1]; dst[2] = px[0];
            dst[3] = bytes_per_pixel == 32 / 8 ? px[3] : 0xff;
        }
    }
    // A number of import tools emit an unused zero alpha byte for 32-bit
    // BMPs. Treat that as the native opaque sentinel, matching STP import.
    bool any_alpha = false;
    for (std::size_t i = 3; i < out.rgba8.size(); i += 4)
        any_alpha = any_alpha || out.rgba8[i] != 0;
    if (bytes_per_pixel == 4 && !any_alpha)
        for (std::size_t i = 3; i < out.rgba8.size(); i += 4)
            out.rgba8[i] = 0xff;
    return out;
}

}  // namespace opennr::render
