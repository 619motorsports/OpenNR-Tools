// lyt_dump — tiny CLI that prints the widget list of one LYT file.
// Used to RE shipped dialogs (widget names, type codes, captions).
//
//   opennr_lyt_dump <path.lyt>
//
// Output: one line per widget — type code, position, name, caption_id.

#include "fs/lyt_layout.h"

#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <path.lyt>\n", argv[0]);
        return 1;
    }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    f.seekg(0, std::ios::end);
    auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> buf(n);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(n));

    opennr::LytLayout lyt;
    try {
        lyt = opennr::LytLayout::parse(buf);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "parse error: %s\n", e.what());
        return 1;
    }
    std::printf("%s: %zu widgets, version=%u rec=%u\n",
                argv[1], lyt.widgets.size(),
                lyt.version, lyt.record_size);
    for (std::size_t i = 0; i < lyt.widgets.size(); ++i) {
        const auto& w = lyt.widgets[i];
        std::printf("  [%3zu] type=%2u pos=(%4d,%4d) size=(%4d,%4d) "
                    "parent=%d pgid=%d ogid=%d anchor=%u align=%u "
                    "params=(%u,%u,%u,%u,%u) "
                    "font=`%s` name=`%s` cap=%u tip=%u tex=`%s` "
                    "art=`%s` slot0=`%s`\n",
                     i, w.type, w.x, w.y, w.width, w.height,
                     w.parent_index, int(w.parent_group_id), int(w.own_group_id),
                     unsigned(w.anchor_code), w.alignment,
                     w.param_min, w.param_max, w.param_default,
                     w.param_axis, w.param_aux, w.fonts[0].c_str(),
                     w.name.c_str(), w.caption_id, w.tooltip_id,
                     w.texture_path.c_str(), w.widget_art_base.c_str(),
                     w.style_slots[0].c_str());
        for (std::size_t slot = 1; slot < w.style_slots.size(); ++slot) {
            if (!w.style_slots[slot].empty()) {
                std::printf("        slot%zu=`%s`\n", slot,
                            w.style_slots[slot].c_str());
            }
        }
        if (w.type == 19) {
            std::printf("        columns=");
            for (std::size_t c = 0; c < w.table_columns.size(); ++c) {
                const auto& column = w.table_columns[c];
                std::printf("%s(w=%d,x=%d,a=%u)", c ? "," : "",
                            int(column.width), int(column.anchor_x),
                            unsigned(column.alignment));
            }
            std::printf("\n");
        }
        if (w.type == 9 && w.anim_frame_count != 0) {
            std::printf("        anim=count:%u duration:%g "
                        "f0=(%d,%d)->(%d,%d) "
                        "f1=(%d,%d)->(%d,%d)\n",
                        unsigned(w.anim_frame_count),
                        double(w.anim_frame_duration),
                        w.anim_frame_rects[0][0], w.anim_frame_rects[0][1],
                        w.anim_frame_rects[0][2], w.anim_frame_rects[0][3],
                        w.anim_frame_rects[1][0], w.anim_frame_rects[1][1],
                        w.anim_frame_rects[1][2], w.anim_frame_rects[1][3]);
        }
    }
    return 0;
}
