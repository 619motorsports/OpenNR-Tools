// Software-renders a string with a BFF font to a BMP, exercising the
// same baseline math that UiRenderer::draw_text uses (without pulling
// in D3D11).  Used to visually verify the per-glyph y-descent + bearing
// math.
//
// usage: opennr_bff_render_text <font.bff> <text> <out.bmp>

#include "fs/bff_font.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    auto sz = in.tellg(); in.seekg(0);
    std::vector<std::uint8_t> v(static_cast<std::size_t>(sz));
    if (sz > 0) in.read(reinterpret_cast<char*>(v.data()),
                       std::streamsize(sz));
    return v;
}

// Writes a 24-bpp uncompressed BMP. Pixels passed as packed BGR rows,
// top-down (we'll flip during write since BMP is bottom-up).
bool save_bmp(const char* path, const std::uint8_t* bgr_top_down, int w, int h) {
    int row_stride = (w * 3 + 3) & ~3;
    std::uint32_t img_size = std::uint32_t(row_stride) * h;
    std::uint32_t file_size = 54 + img_size;

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto put16 = [&](std::uint16_t v) {
        std::uint8_t b[2] = {std::uint8_t(v), std::uint8_t(v >> 8)};
        f.write((const char*)b, 2);
    };
    auto put32 = [&](std::uint32_t v) {
        std::uint8_t b[4] = {std::uint8_t(v), std::uint8_t(v >> 8),
                             std::uint8_t(v >> 16), std::uint8_t(v >> 24)};
        f.write((const char*)b, 4);
    };
    // File header
    f.write("BM", 2);
    put32(file_size);
    put16(0); put16(0);
    put32(54);
    // DIB header
    put32(40);
    put32(std::uint32_t(w));
    put32(std::uint32_t(h));
    put16(1);
    put16(24);
    put32(0);
    put32(img_size);
    put32(2835); put32(2835);
    put32(0); put32(0);
    // Pixels, bottom-up
    std::vector<std::uint8_t> row(row_stride, 0);
    for (int y = h - 1; y >= 0; --y) {
        std::memcpy(row.data(), bgr_top_down + std::size_t(y) * w * 3,
                    std::size_t(w) * 3);
        f.write((const char*)row.data(), row_stride);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: bff_render_text <font.bff> <text> <out.bmp>\n");
        return 2;
    }
    auto bytes = read_file(argv[1]);
    if (bytes.empty()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    auto font = opennr::BffFont::parse(bytes);
    const char* text = argv[2];

    const int line_h  = int(font.line_height_px);
    const int descent = int(font.descent_below_baseline_px);

    int pen = 0, min_top = 0, max_bottom = line_h;
    int run_top = 0;
    int baseline = run_top + line_h - descent;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        const auto* g = font.find_glyph(*p);
        if (!g) continue;
        if (g->atlas_w > 0 && g->atlas_h > 0) {
            int bottom = baseline + g->y_descent_px;
            int top    = bottom - int(g->atlas_h);
            if (top    < min_top)    min_top    = top;
            if (bottom > max_bottom) max_bottom = bottom;
        }
        pen += int(g->advance_px > 0.f ? g->advance_px : g->atlas_w);
    }

    int margin = 6;
    int W = pen + margin * 2;
    int H = (max_bottom - min_top) + margin * 2 + 4;
    if (W < 32) W = 32; if (H < 32) H = 32;
    int origin_y = -min_top + margin;

    std::vector<std::uint8_t> bgr(std::size_t(W) * H * 3, 0);
    for (int i = 0; i < W * H; ++i) {
        bgr[i*3 + 0] = 0x30;  // B
        bgr[i*3 + 1] = 0x22;  // G
        bgr[i*3 + 2] = 0x18;  // R
    }
    int marked_baseline = baseline + origin_y;
    if (marked_baseline >= 0 && marked_baseline < H) {
        for (int x = 0; x < W; ++x) {
            auto* p2 = &bgr[(std::size_t(marked_baseline) * W + x) * 3];
            p2[0] = 0x60; p2[1] = 0x40; p2[2] = 0x40;
        }
    }
    int marked_top = run_top + origin_y;
    if (marked_top >= 0 && marked_top < H) {
        for (int x = 0; x < W; ++x) {
            auto* p2 = &bgr[(std::size_t(marked_top) * W + x) * 3];
            p2[0] = 0x20; p2[1] = 0x20; p2[2] = 0x40;
        }
    }
    int marked_bottom = (run_top + line_h) + origin_y;
    if (marked_bottom >= 0 && marked_bottom < H) {
        for (int x = 0; x < W; ++x) {
            auto* p2 = &bgr[(std::size_t(marked_bottom) * W + x) * 3];
            p2[0] = 0x20; p2[1] = 0x40; p2[2] = 0x20;
        }
    }

    pen = margin;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        const auto* g = font.find_glyph(*p);
        if (!g) continue;
        if (g->atlas_w > 0 && g->atlas_h > 0) {
            int bottom = baseline + g->y_descent_px;
            int top    = bottom - int(g->atlas_h);
            int dst_x  = pen + int(g->bearing_px);
            int dst_y  = top + origin_y;
            for (int gy = 0; gy < int(g->atlas_h); ++gy) {
                int yy = dst_y + gy;
                if (yy < 0 || yy >= H) continue;
                for (int gx = 0; gx < int(g->atlas_w); ++gx) {
                    int xx = dst_x + gx;
                    if (xx < 0 || xx >= W) continue;
                    auto a = font.atlas_alpha[
                        std::size_t(g->atlas_y + gy) * font.atlas_w +
                        std::size_t(g->atlas_x + gx)];
                    if (a == 0) continue;
                    auto* dst = &bgr[(std::size_t(yy) * W + xx) * 3];
                    int alpha = a;
                    int inv   = 255 - alpha;
                    dst[0] = std::uint8_t((255 * alpha + dst[0] * inv) / 255);
                    dst[1] = std::uint8_t((255 * alpha + dst[1] * inv) / 255);
                    dst[2] = std::uint8_t((255 * alpha + dst[2] * inv) / 255);
                }
            }
        }
        pen += int(g->advance_px > 0.f ? g->advance_px : g->atlas_w);
    }

    if (!save_bmp(argv[3], bgr.data(), W, H)) {
        std::fprintf(stderr, "could not write %s\n", argv[3]);
        return 1;
    }
    std::printf("wrote %s (%dx%d)  baseline=%d  line_h=%d  descent=%d\n",
                argv[3], W, H, baseline + origin_y, line_h, descent);
    return 0;
}
