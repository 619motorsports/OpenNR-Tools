#include "fs/tga_image.h"

#include <limits>
#include <stdexcept>

namespace opennr {

namespace {

std::uint16_t read_u16(std::span<const std::uint8_t> bytes,
                       std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

void write_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

std::uint8_t expand5(std::uint16_t value) {
    return static_cast<std::uint8_t>((value << 3) | (value >> 2));
}

}  // namespace

TgaImage TgaImage::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 18) throw std::runtime_error("TGA: truncated header");

    const std::uint8_t id_length = bytes[0];
    const std::uint8_t color_map_type = bytes[1];
    const std::uint8_t image_type = bytes[2];
    const std::uint32_t width = read_u16(bytes, 12);
    const std::uint32_t height = read_u16(bytes, 14);
    const std::uint8_t depth = bytes[16];
    const std::uint8_t descriptor = bytes[17];
    if (color_map_type != 0 || (image_type != 2 && image_type != 10))
        throw std::runtime_error("TGA: unsupported image type");
    if (width == 0 || height == 0)
        throw std::runtime_error("TGA: invalid dimensions");
    if (width > 4096 || height > 4096)
        throw std::runtime_error("TGA: dimensions exceed texture limit");
    if (depth != 15 && depth != 16 && depth != 24 && depth != 32)
        throw std::runtime_error("TGA: unsupported pixel depth");
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 4)
        throw std::runtime_error("TGA: dimensions overflow");

    std::size_t cursor = 18u + id_length;
    if (cursor > bytes.size()) throw std::runtime_error("TGA: truncated ID");
    const std::size_t bytes_per_pixel = depth <= 16 ? 2u : depth / 8u;
    std::vector<std::uint8_t> source(pixel_count * 4);

    auto read_pixel = [&](std::uint8_t* destination) {
        if (cursor + bytes_per_pixel > bytes.size())
            throw std::runtime_error("TGA: truncated pixel data");
        if (depth <= 16) {
            const std::uint16_t value = read_u16(bytes, cursor);
            destination[0] = expand5((value >> 10) & 0x1f);  // R
            destination[1] = expand5((value >> 5) & 0x1f);   // G
            destination[2] = expand5(value & 0x1f);          // B
            destination[3] = depth == 16 && (descriptor & 0x0f) != 0
                ? ((value & 0x8000) ? 0xff : 0x00) : 0xff;
        } else {
            destination[0] = bytes[cursor + 2];
            destination[1] = bytes[cursor + 1];
            destination[2] = bytes[cursor + 0];
            destination[3] = depth == 32 ? bytes[cursor + 3] : 0xff;
        }
        cursor += bytes_per_pixel;
    };

    std::size_t emitted = 0;
    if (image_type == 2) {
        while (emitted < pixel_count) {
            read_pixel(source.data() + emitted * 4);
            ++emitted;
        }
    } else {
        while (emitted < pixel_count) {
            if (cursor >= bytes.size())
                throw std::runtime_error("TGA: truncated RLE packet");
            const std::uint8_t packet = bytes[cursor++];
            const std::size_t count = (packet & 0x7f) + 1u;
            if (count > pixel_count - emitted)
                throw std::runtime_error("TGA: RLE packet overruns image");
            if ((packet & 0x80) != 0) {
                std::uint8_t pixel[4];
                read_pixel(pixel);
                for (std::size_t index = 0; index < count; ++index) {
                    for (int channel = 0; channel < 4; ++channel)
                        source[(emitted + index) * 4 + channel] = pixel[channel];
                }
            } else {
                for (std::size_t index = 0; index < count; ++index)
                    read_pixel(source.data() + (emitted + index) * 4);
            }
            emitted += count;
        }
    }

    TgaImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(pixel_count * 4);
    const bool right_origin = (descriptor & 0x10) != 0;
    const bool top_origin = (descriptor & 0x20) != 0;
    for (std::uint32_t source_y = 0; source_y < height; ++source_y) {
        const std::uint32_t y = top_origin ? source_y : height - source_y - 1;
        for (std::uint32_t source_x = 0; source_x < width; ++source_x) {
            const std::uint32_t x = right_origin
                ? width - source_x - 1 : source_x;
            const std::size_t from =
                (static_cast<std::size_t>(source_y) * width + source_x) * 4;
            const std::size_t to =
                (static_cast<std::size_t>(y) * width + x) * 4;
            for (int channel = 0; channel < 4; ++channel)
                image.rgba[to + channel] = source[from + channel];
        }
    }
    return image;
}

std::vector<std::uint8_t> TgaImage::serialize_24bit_top_left() const {
    if (width == 0 || height == 0 || width > 65535 || height > 65535 ||
        rgba.size() != static_cast<std::size_t>(width) * height * 4) {
        throw std::runtime_error("TGA: invalid image");
    }
    std::vector<std::uint8_t> out;
    out.reserve(18 + static_cast<std::size_t>(width) * height * 3);
    out.push_back(0);             // ID length
    out.push_back(0);             // no colour map
    out.push_back(2);             // uncompressed true colour
    for (int index = 0; index < 5; ++index) out.push_back(0);
    write_u16(out, 0);            // x origin
    write_u16(out, 0);            // y origin
    write_u16(out, static_cast<std::uint16_t>(width));
    write_u16(out, static_cast<std::uint16_t>(height));
    out.push_back(24);
    out.push_back(0x20);          // top-left origin
    for (std::size_t index = 0; index < rgba.size(); index += 4) {
        out.push_back(rgba[index + 2]);
        out.push_back(rgba[index + 1]);
        out.push_back(rgba[index + 0]);
    }
    return out;
}

}  // namespace opennr
