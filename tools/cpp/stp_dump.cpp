// Smoke tool: decode an .stp file and write the RGBA buffer as a
// no-frills BMP for visual inspection.  Used to debug the colour
// channel order on copyright.stp.

#include "fs/stp_image.h"
#include "render/image_loader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {
std::vector<std::uint8_t> read_all(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> b(n);
    f.read(reinterpret_cast<char*>(b.data()),
           static_cast<std::streamsize>(n));
    return b;
}

void write_bmp_rgba(const char* path,
                    int w, int h,
                    const std::uint8_t* rgba) {
    // 32bpp BGRA bitmap (rows bottom-up).
    std::uint32_t row = static_cast<std::uint32_t>(w) * 4;
    std::uint32_t pixels = row * h;
    std::uint32_t file = 14 + 40 + pixels;
    std::ofstream f(path, std::ios::binary);
    auto u16 = [&](std::uint16_t v) {
        char b[2] = {char(v & 0xff), char((v >> 8) & 0xff)};
        f.write(b, 2);
    };
    auto u32 = [&](std::uint32_t v) {
        char b[4] = {char(v & 0xff), char((v >> 8) & 0xff),
                     char((v >> 16) & 0xff), char((v >> 24) & 0xff)};
        f.write(b, 4);
    };
    // BMP header
    f.put('B'); f.put('M');
    u32(file); u32(0); u32(54);
    // DIB header
    u32(40); u32(static_cast<std::uint32_t>(w)); u32(static_cast<std::uint32_t>(h));
    u16(1); u16(32); u32(0); u32(pixels); u32(2835); u32(2835); u32(0); u32(0);
    // Pixels — bottom-up, RGBA→BGRA
    std::vector<std::uint8_t> tmp(row);
    for (int y = h - 1; y >= 0; --y) {
        const std::uint8_t* src = rgba + std::size_t(y) * row;
        for (int x = 0; x < w; ++x) {
            tmp[x * 4 + 0] = src[x * 4 + 2]; // B = source.b
            tmp[x * 4 + 1] = src[x * 4 + 1]; // G = source.g
            tmp[x * 4 + 2] = src[x * 4 + 0]; // R = source.r
            tmp[x * 4 + 3] = src[x * 4 + 3]; // A
        }
        f.write(reinterpret_cast<const char*>(tmp.data()), row);
    }
}
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: opennr_stp_dump <in.stp> <out.bmp>\n");
        return 2;
    }
    auto b = read_all(argv[1]);
    if (b.empty()) { std::fprintf(stderr, "read failed\n"); return 1; }
    try {
        auto img = opennr::StpImage::parse(b);
        std::printf("stp: %ux%u fmt=0x%02x pitch=%u n_mips=%u\n",
                    img.width, img.height, img.format, img.pitch, img.n_mips);
        std::printf("  subimages=%u:", unsigned(img.sthd_count));
        for (const auto& sub : img.subimages) {
            std::printf(" %ux%u", unsigned(sub.width), unsigned(sub.height));
        }
        std::printf("\n");
        // Print a sample of the raw decompressed BGR/RGB triples from
        // the .stp body so we can see what the on-disk byte order is.
        std::printf("raw pre-decode first 30 bytes:");
        for (int i = 0; i < 30 && i < int(img.pixels.size()); ++i) {
            std::printf(" %02x", img.pixels[i]);
        }
        std::printf("\n");
        // The Papyrus copyright top-left pixel of the .stp content area
        // is at row 0; an orange section is near the "PAPYRUS" letters
        // around row 130, col 220 — pull samples for visual inspection.
        auto raw_pixel = [&](int x, int y) {
            if (x < 0 || y < 0 ||
                x >= int(img.width) || y >= int(img.height)) return;
            std::size_t bpp = 3;
            if (img.format == 0x07) bpp = 4;
            const std::uint8_t* p = img.pixels.data()
                + std::size_t(y) * img.pitch + bpp * x;
            std::printf("  src(%d,%d) bytes=", x, y);
            for (std::size_t i = 0; i < bpp; ++i) std::printf(" %02x", p[i]);
            std::printf("\n");
        };
        raw_pixel(0, 0);
        raw_pixel(int(img.width) / 2, int(img.height) / 2);
        raw_pixel(int(img.width) - 1, int(img.height) - 1);
        auto dec = opennr::render::decode_stp_rgba8(img);
        std::printf("decoded: %ux%u bytes=%zu\n",
                    dec.width, dec.height, dec.rgba8.size());
        if (dec.width == 0) return 1;
        auto rgba_pixel = [&](int x, int y) {
            if (x < 0 || y < 0 ||
                x >= int(dec.width) || y >= int(dec.height)) return;
            const std::uint8_t* p = dec.rgba8.data()
                + (std::size_t(y) * dec.width + x) * 4;
            std::printf("  rgba(%d,%d) r=%u g=%u b=%u a=%u\n",
                        x, y, p[0], p[1], p[2], p[3]);
        };
        rgba_pixel(0, 0);
        rgba_pixel(int(dec.width) / 2, int(dec.height) / 2);
        rgba_pixel(int(dec.width) - 1, int(dec.height) - 1);
        write_bmp_rgba(argv[2], static_cast<int>(dec.width),
                       static_cast<int>(dec.height), dec.rgba8.data());
        std::printf("wrote %s\n", argv[2]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
