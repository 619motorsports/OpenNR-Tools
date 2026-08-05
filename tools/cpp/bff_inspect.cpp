// Inspect a .bff file via the C++ parser - prints per-glyph metrics
// and atlas dimensions, plus dumps the decoded 8-bit atlas to a raw
// file for visual comparison.

#include "fs/bff_font.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: bff_inspect <file.bff> [out.gray]\n"); return 2; }
    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    auto sz_pos = in.tellg(); in.seekg(0);
    std::size_t sz = static_cast<std::size_t>(sz_pos);
    std::vector<std::uint8_t> bytes(sz);
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(sz));
    auto f = opennr::BffFont::parse(bytes);

    std::printf("line_h=%u  descent=%u  flags=0x%x  glyphs=%zu  atlas=%ux%u\n",
                f.line_height_px, f.descent_below_baseline_px, f.pfhd_flags,
                f.glyphs.size(), f.atlas_w, f.atlas_h);
    std::printf("First 8 glyphs (from C++ parser):\n");
    for (std::size_t i = 0; i < 8 && i < f.glyphs.size(); ++i) {
        const auto& g = f.glyphs[i];
        std::printf("  cp=0x%04x  bear=%.2f  adv=%.2f  ydesc=%d  wpx=%u  atlas=(%u,%u)+(%u,%u)\n",
                    g.codepoint, g.bearing_px, g.advance_px,
                    static_cast<int>(g.y_descent_px), g.width_px,
                    g.atlas_x, g.atlas_y, g.atlas_w, g.atlas_h);
    }
    if (argc >= 3) {
        std::ofstream out(argv[2], std::ios::binary);
        out.write(reinterpret_cast<const char*>(f.atlas_alpha.data()),
                  std::streamsize(f.atlas_alpha.size()));
        std::printf("wrote %zu bytes of 8-bit atlas to %s\n",
                    f.atlas_alpha.size(), argv[2]);
    }
    return 0;
}
