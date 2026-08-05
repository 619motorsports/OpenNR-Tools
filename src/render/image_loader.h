#pragma once

// Helpers that turn a parsed .stp or .mip file into an RGBA8 byte
// buffer ready for a 2D texture upload. The pixel-format byte is
// shared between the two containers; both fall through to
// mip_decode_to_rgba8888 once we have a MipLevel-shaped view.
//
// Lives in src/render because the natural caller is the UiRenderer
// texture-cache path; the .stp/.mip parsers themselves live in
// src/fs and intentionally don't depend on the renderer.

#include "fs/mip_texture.h"
#include "fs/stp_image.h"
#include "render/types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace opennr::render {

struct DecodedImage {
    std::uint32_t             width  = 0;
    std::uint32_t             height = 0;
    std::vector<std::uint8_t> rgba8;  // size == width * height * 4
    AddressMode address_u = AddressMode::clamp;
    AddressMode address_v = AddressMode::clamp;
};

// Decode an .stp file (already DCL-decompressed by StpImage::parse) into
// a tightly-packed RGBA8 buffer.  Throws std::runtime_error if the .stp
// pixel format byte is one we don't handle.
DecodedImage decode_stp_rgba8(const StpImage& img);

// Same as above, parsing the bytes first.
DecodedImage decode_stp_rgba8(std::span<const std::uint8_t> bytes);

// Decode the largest level of an .mip file into RGBA8.
DecodedImage decode_mip_rgba8(const MipTexture& tex);
DecodedImage decode_mip_rgba8(std::span<const std::uint8_t> bytes);

// Decode the external image forms accepted by the replay-editor Stamp
// resource loader. These are intentionally kept beside the STP/MIP upload
// helpers so UiRenderer and replay overlays share one RGBA8 boundary.
DecodedImage decode_tga_rgba8(std::span<const std::uint8_t> bytes);
DecodedImage decode_bmp_rgba8(std::span<const std::uint8_t> bytes);

}  // namespace opennr::render
