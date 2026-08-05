#include "object_tree.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace opennr {

namespace {

// Same plausibility test as the structural walker — keeps the two
// codepaths agreeing on what counts as a token.
bool is_plausible_token(std::span<const std::uint8_t> bytes,
                         std::size_t pos, std::uint32_t n) {
    if (n < 2 || n > 64) return false;
    if (pos + 4 + n > bytes.size()) return false;
    if (bytes[pos + 4 + n - 1] != 0) return false;   // NUL-terminated
    if (bytes[pos + 4]         == 0) return false;
    for (std::uint32_t i = 0; i < n - 1; ++i) {
        std::uint8_t b = bytes[pos + 4 + i];
        if (b < 0x20 || b >= 0x7F) return false;
    }
    auto first = bytes[pos + 4];
    if (first != '_' && first != '(' &&
        !((first >= 'a' && first <= 'z') ||
          (first >= 'A' && first <= 'Z'))) {
        return false;
    }
    return true;
}

std::uint32_t read_u32(std::span<const std::uint8_t> b, std::size_t p) {
    if (p + 4 > b.size()) return 0;
    return std::uint32_t(b[p]) |
           (std::uint32_t(b[p + 1]) <<  8) |
           (std::uint32_t(b[p + 2]) << 16) |
           (std::uint32_t(b[p + 3]) << 24);
}

std::int32_t read_i32(std::span<const std::uint8_t> b, std::size_t p) {
    return static_cast<std::int32_t>(read_u32(b, p));
}

[[maybe_unused]] float read_f32_unaligned(
    std::span<const std::uint8_t> b, std::size_t p) {
    float v = 0;
    if (p + 4 > b.size()) return v;
    std::memcpy(&v, b.data() + p, sizeof(v));
    return v;
}

double read_f64_unaligned(std::span<const std::uint8_t> b, std::size_t p) {
    // The descriptor body fields can sit at unaligned offsets (see
    // `docs/formats/3do_descriptor_layouts.md` PointLightDescriptor)
    // — copy 8 bytes through a memcpy so we don't trip the strict
    // alignment rule on hosts that care.
    double v = 0;
    if (p + 8 > b.size()) return v;
    std::memcpy(&v, b.data() + p, sizeof(v));
    return v;
}

// Find the next plausible class-name token at or after `pos`; returns
// bytes.size() if none.  Used to bracket descriptor bodies of unknown
// size.
std::size_t find_next_class_token(std::span<const std::uint8_t> bytes,
                                   std::size_t pos) {
    while (pos + 4 <= bytes.size()) {
        std::uint32_t n = read_u32(bytes, pos);
        if (is_plausible_token(bytes, pos, n)) {
            std::string s(reinterpret_cast<const char *>(&bytes[pos + 4]),
                          n - 1);
            if (is_known_object_class(s)) return pos;
            if (s.size() >= 4 &&
                (s.compare(s.size() - 4, 4, ".mip") == 0 ||
                 s.compare(s.size() - 4, 4, ".3do") == 0)) {
                return pos;
            }
        }
        ++pos;
    }
    return bytes.size();
}

// Alias for the shared find_next_class_token helper.
std::size_t find_next_token(std::span<const std::uint8_t> bytes, std::size_t pos) {
    return find_next_class_token(bytes, pos);
}

// Attempt to decode a per-class body payload.  Returns true iff we
// recognised the class and populated `out`.  `body_pos` is the byte
// offset of the first byte AFTER the class-name string (length prefix
// + chars + NUL).  `next_token_pos` is the byte offset of the next
// class-token in the stream (used to bracket variable-size bodies).
bool decode_body(std::span<const std::uint8_t> bytes, std::size_t body_pos,
                  std::size_t next_token_pos,
                  const std::string &class_name, ObjectNode &out) {
    auto remaining = bytes.size() - body_pos;
    if (remaining < 8) return false;

    DescriptorHeader hdr = decode_descriptor_header(bytes, body_pos);
    const std::size_t after_hdr = body_pos + hdr.header_bytes;

    if (class_name == "GroupDescriptor") {
        GroupDescriptor d;
        d.header = hdr;
        if (after_hdr + 8 <= bytes.size()) {
            d.flag_a = read_u32(bytes, after_hdr);
            d.flag_b = read_u32(bytes, after_hdr + 4);
        }
        out.descriptor = d;

        // Legacy payload: best-effort num_children for the count-driven
        // recursion in parse_node.  Across the shipped Atlanta `.3do`
        // sample the children-count immediately follows a 48-byte
        // zero-run (presumed 6-double bbox).  We scan for the first
        // small u32 after the header that is followed by what looks
        // like a `length-prefix + ClassName` token, and use that as
        // the count.
        GroupPayload p;
        p.num_children = 0;
        // The body extends until next_token_pos -- search for a small
        // positive count that yields next_token_pos directly after it.
        for (std::size_t scan = after_hdr;
             scan + 4 <= next_token_pos; scan += 4) {
            std::uint32_t v = read_u32(bytes, scan);
            if (v > 0 && v < 1024) {
                // If a token starts at scan+4, this is our num_children.
                if (scan + 4 == next_token_pos) {
                    p.num_children = v;
                    d.num_children = v;
                    break;
                }
            }
        }
        out.payload = p;
        out.descriptor = d;
        return true;
    }

    if (class_name == "GroupingNodeDescriptor") {
        GroupingNodeDescriptor d;
        d.header = hdr;
        if (after_hdr + 8 <= bytes.size()) {
            d.flag_a = read_u32(bytes, after_hdr);
            d.flag_b = read_u32(bytes, after_hdr + 4);
        }
        for (std::size_t scan = after_hdr;
             scan + 4 <= next_token_pos; scan += 4) {
            std::uint32_t v = read_u32(bytes, scan);
            if (v > 0 && v < 1024 && scan + 4 == next_token_pos) {
                d.num_children = v;
                break;
            }
        }
        out.descriptor = d;
        return true;
    }

    if (class_name == "LodSwitchDescriptor") {
        LodSwitchDescriptor d;
        d.header = hdr;
        // First i32 after the header is num_lod_levels (best-effort;
        // capped to a plausible range).
        std::int32_t n = read_i32(bytes, after_hdr);
        if (n >= 0 && n <= 64) d.num_lod_levels = n;
        out.descriptor = d;
        return true;
    }

    if (class_name == "StateSwitchDescriptor") {
        StateSwitchDescriptor d;
        d.header = hdr;
        std::int32_t n = read_i32(bytes, after_hdr);
        if (n >= 0 && n <= 64) d.num_states = n;
        out.descriptor = d;
        return true;
    }

    if (class_name == "TransformDescriptor") {
        TransformDescriptor d;
        d.header = hdr;
        // Confirmed across 9+ shipped files (catch_can_man, gas_man,
        // helper1, flagger_*, etc.):
        //   after universal_header (which itself carries the node name):
        //     u32 flag_a = 1
        //     u32 flag_b = 1
        //     48 bytes (presumed 6 zero doubles -- initial bbox)
        //     u32 marker = 1
        //     6 UNALIGNED doubles: tx, ty, tz, then rotation triple.
        const std::size_t doubles_off = after_hdr + 4 + 4 + 48 + 4;
        if (doubles_off + 48 <= bytes.size() &&
            read_u32(bytes, after_hdr) == 1 &&
            read_u32(bytes, after_hdr + 4) == 1 &&
            read_u32(bytes, after_hdr + 4 + 4 + 48) == 1) {
            double vs[6];
            bool ok = true;
            for (int i = 0; i < 6; ++i) {
                vs[i] = read_f64_unaligned(bytes, doubles_off + i * 8);
                if (!(vs[i] >= -1e6 && vs[i] <= 1e6) || vs[i] != vs[i]) {
                    ok = false; break;
                }
            }
            if (ok) {
                d.tx = vs[0]; d.ty = vs[1]; d.tz = vs[2];
                d.yaw = vs[3]; d.pitch = vs[4]; d.roll = vs[5];
                d.fields_decoded = true;
            }
        }
        TransformPayload p{d.tx, d.ty, d.tz, d.yaw, d.pitch, d.roll};
        out.payload = p;
        out.descriptor = d;
        return true;
    }

    if (class_name == "AnimatedTransformDescriptor") {
        AnimatedTransformDescriptor d;
        d.header = hdr;
        // Same scan strategy as TransformDescriptor.
        for (std::size_t scan = after_hdr;
             scan + 48 <= next_token_pos; ++scan) {
            bool ok = true;
            double vs[6] = {0};
            for (int i = 0; i < 6; ++i) {
                double v = read_f64_unaligned(bytes, scan + i * 8);
                if (!(v >= -1e6 && v <= 1e6) || v != v) {
                    ok = false; break;
                }
                vs[i] = v;
            }
            if (ok && (vs[0] != 0.0 || vs[1] != 0.0 || vs[2] != 0.0 ||
                       vs[3] != 0.0 || vs[4] != 0.0 || vs[5] != 0.0)) {
                d.tx = vs[0]; d.ty = vs[1]; d.tz = vs[2];
                d.axis_x = vs[3]; d.axis_y = vs[4]; d.axis_z = vs[5];
                d.fields_decoded = true;
                break;
            }
        }
        out.descriptor = d;
        return true;
    }

    if (class_name == "BillboardDescriptor") {
        BillboardDescriptor d;
        d.header = hdr;
        out.descriptor = d;
        return true;
    }

    if (class_name == "PortalDescriptor") {
        PortalDescriptor d;
        d.header = hdr;
        out.descriptor = d;
        return true;
    }

    if (class_name == "PointLightDescriptor" ||
        class_name == "InfiniteLightDescriptor") {
        PointLightDescriptor d;
        d.header = hdr;
        out.descriptor = d;
        return true;
    }

    if (class_name == "AppearanceDescriptor") {
        AppearanceDescriptor d;
        d.header = hdr;
        out.descriptor = d;
        return true;
    }

    if (class_name == "ProgressiveModificationDescriptor") {
        ProgressiveModificationDescriptor d;
        d.header = hdr;
        if (after_hdr + 16 <= bytes.size()) {
            d.change_num_vertices   = read_i32(bytes, after_hdr);
            d.num_modified_vertices = read_i32(bytes, after_hdr + 4);
            d.change_num_tris       = read_i32(bytes, after_hdr + 8);
            d.num_modified_tris     = read_i32(bytes, after_hdr + 12);
            d.fields_decoded = true;
        }
        out.descriptor = d;
        return true;
    }

    if (class_name == "TrackDescriptor") {
        TrackDescriptor d;
        d.header = hdr;
        // Body: u32 type, u32 num_segments
        if (after_hdr + 8 <= bytes.size()) {
            d.type_code    = read_u32(bytes, after_hdr);
            d.num_segments = read_i32(bytes, after_hdr + 4);
            if (d.num_segments < 0 || d.num_segments > 10000)
                d.num_segments = 0;
        }
        TrackPayload p;
        p.num_segments = static_cast<std::uint32_t>(d.num_segments);
        out.payload = p;
        out.descriptor = d;
        return true;
    }

    if (class_name == "SegmentDescriptor") {
        SegmentDescriptor d;
        d.header = hdr;
        std::size_t cursor = after_hdr;
        auto safe_u32 = [&](std::uint32_t& dst) -> bool {
            if (cursor + 4 > next_token_pos) return false;
            dst = read_u32(bytes, cursor);
            cursor += 4;
            return true;
        };
        auto read_ref_list = [&](std::vector<std::uint32_t>& list) -> bool {
            std::uint32_t count = 0;
            if (!safe_u32(count)) return false;
            if (count > 1024) return false;     // sanity bound
            list.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t v;
                if (!safe_u32(v)) return false;
                list.push_back(v);
            }
            return true;
        };
        if (safe_u32(d.type_code) &&
            read_ref_list(d.x_section_refs) &&
            read_ref_list(d.f_section_refs) &&
            read_ref_list(d.w_section_refs)) {
            std::uint32_t kind_raw;
            if (safe_u32(kind_raw)) {
                d.segment_kind = static_cast<std::int32_t>(kind_raw);
                if (d.segment_kind != -1 &&
                    cursor + 6 * 8 + 2 <= next_token_pos) {
                    d.pos_a   = read_f64_unaligned(bytes, cursor); cursor += 8;
                    d.pos_b   = read_f64_unaligned(bytes, cursor); cursor += 8;
                    d.angle_c = read_f64_unaligned(bytes, cursor); cursor += 8;
                    d.pos_d   = read_f64_unaligned(bytes, cursor); cursor += 8;
                    d.pos_e   = read_f64_unaligned(bytes, cursor); cursor += 8;
                    d.angle_f = read_f64_unaligned(bytes, cursor); cursor += 8;
                    if (cursor + 2 <= next_token_pos) {
                        d.flag_a = bytes[cursor++];
                        d.flag_b = bytes[cursor++];
                    }
                    std::uint32_t n12 = 0, n16 = 0;
                    if (safe_u32(n12) && n12 < 4096 &&
                        cursor + n12 * 12 <= next_token_pos) {
                        d.records_12_raw.assign(
                            bytes.begin() + cursor,
                            bytes.begin() + cursor + n12 * 12);
                        cursor += n12 * 12;
                        if (safe_u32(n16) && n16 < 4096 &&
                            cursor + n16 * 16 <= next_token_pos) {
                            d.records_16_raw.assign(
                                bytes.begin() + cursor,
                                bytes.begin() + cursor + n16 * 16);
                            cursor += n16 * 16;
                        }
                    }
                }
                d.fields_decoded = true;
            }
        }
        out.descriptor = d;
        return true;
    }

    if (class_name == "X_SectionDescriptor") {
        X_SectionDescriptor d;
        d.header = hdr;
        std::size_t cursor = after_hdr;
        // u32 type_code (== 3)
        if (cursor + 4 <= bytes.size()) {
            d.type_code = read_u32(bytes, cursor);
            cursor += 4;
        }
        const std::size_t scalar_bytes = d.type_code < 3 ? 8 : 4;
        const std::size_t required = scalar_bytes * 4 + 16;
        if (cursor + required <= bytes.size()) {
            auto read_versioned_float = [&]() {
                float value = d.type_code < 3
                    ? static_cast<float>(read_f64_unaligned(bytes, cursor))
                    : read_f32_unaligned(bytes, cursor);
                cursor += scalar_bytes;
                return value;
            };
            d.lateral_start = read_versioned_float();
            d.height_start = read_f64_unaligned(bytes, cursor); cursor += 8;
            d.slope_start = read_versioned_float();
            d.lateral_end = read_versioned_float();
            d.height_end = read_f64_unaligned(bytes, cursor); cursor += 8;
            d.slope_end = read_versioned_float();
            if (d.type_code >= 2 && cursor < bytes.size()) {
                d.visual_curve_mode = bytes[cursor++];
            }
            auto read_seam = [&](X_SectionDescriptor::EndpointSeam& seam) {
                if (cursor >= bytes.size()) return;
                seam.kind = bytes[cursor++];
                if (seam.kind != 0 && cursor + 8 <= bytes.size()) {
                    seam.parameter0 = read_f32_unaligned(bytes, cursor);
                    cursor += 4;
                    seam.parameter1 = read_f32_unaligned(bytes, cursor);
                    cursor += 4;
                }
                if (seam.kind == 2 && cursor + 2 <= bytes.size()) {
                    seam.source_boundary_index = bytes[cursor++];
                    seam.target_boundary_index = bytes[cursor++];
                }
            };
            if (d.type_code >= 3) {
                read_seam(d.start_seam);
                read_seam(d.end_seam);
            }
            d.fields_decoded = true;
        }
        out.descriptor = d;
        return true;
    }

    if (class_name == "F_SectionDescriptor") {
        F_SectionDescriptor d;
        d.header = hdr;
        out.descriptor = d;
        return true;
    }

    if (class_name == "W_SectionDescriptor") {
        W_SectionDescriptor d;
        d.header = hdr;
        out.descriptor = d;
        return true;
    }

    if (class_name == "TSODescriptor") {
        TSODescriptor d;
        d.header = hdr;
        out.descriptor = d;
        return true;
    }

    if (class_name == "TSOReferenceDescriptor") {
        TSOReferenceDescriptor d;
        d.header = hdr;
        out.descriptor = d;
        return true;
    }

    if (class_name == "TrackDetailDescriptor") {
        TrackDetailDescriptor d;
        d.header = hdr;
        out.descriptor = d;
        return true;
    }

    if (class_name == "TextureCoordsDescriptor") {
        TextureCoordsDescriptor d;
        d.header = hdr;
        out.descriptor = d;
        return true;
    }

    if (class_name == "GeometryDescriptor") {
        GeometryDescriptor d;
        d.header = hdr;
        if (after_hdr + 12 <= bytes.size()) {
            d.type_code = read_u32(bytes, after_hdr);
            d.unk_a     = read_u32(bytes, after_hdr + 4);
            d.unk_b     = read_u32(bytes, after_hdr + 8);
        }
        out.descriptor = d;
        return true;
    }

    if (class_name == "ShapeDescriptor") {
        ShapeDescriptor d;
        d.header = hdr;
        out.descriptor = d;
        return true;
    }

    if (class_name == "TextureDescriptor") {
        // Body: u32 type, u32 tex_name_len, char tex_name[len],
        //       25 bytes unknown, 12 doubles UV matrix (unaligned).
        TextureDescriptor d;
        d.header = hdr;
        if (after_hdr + 8 > bytes.size()) {
            out.descriptor = d;
            return true;
        }
        d.type_code = read_u32(bytes, after_hdr);
        std::uint32_t name_len = read_u32(bytes, after_hdr + 4);
        if (name_len >= 2 && name_len <= 96 &&
            after_hdr + 8 + name_len <= bytes.size() &&
            bytes[after_hdr + 8 + name_len - 1] == 0) {
            d.texture_name.assign(
                reinterpret_cast<const char *>(&bytes[after_hdr + 8]),
                name_len - 1);
            std::size_t matrix_start = after_hdr + 8 + name_len + 25;
            if (matrix_start + 96 <= bytes.size()) {
                d.uv_matrix.reserve(12);
                for (int i = 0; i < 12; ++i) {
                    double v = read_f64_unaligned(bytes,
                                                  matrix_start + i * 8);
                    if (!(v == v) || !(v > -1e10 && v < 1e10)) {
                        d.uv_matrix.clear();
                        break;
                    }
                    d.uv_matrix.push_back(v);
                }
            }
        }
        TexturePayload tp;
        tp.name = d.texture_name;
        out.payload = tp;
        out.descriptor = d;
        return true;
    }

    if (class_name == "PlainVertexListDescriptor") {
        PlainVertexListDescriptor d;
        d.header = hdr;
        // Body: u32 type (=1), u32 flag (=1), u32 num_vertices, then
        // 3 position channels, separator, 3 normal channels, u32 num_uv,
        // then num_uv UV channels.  See descriptors.h for the layout.
        if (after_hdr + 12 <= bytes.size()) {
            std::uint32_t type [[maybe_unused]] = read_u32(bytes, after_hdr);
            std::uint32_t flag [[maybe_unused]] = read_u32(bytes, after_hdr + 4);
            std::int32_t  n    = read_i32(bytes, after_hdr + 8);
            if (n >= 0 && n <= 1000000) {
                d.num_vertices = n;
                std::size_t p = after_hdr + 12;
                std::size_t expected =
                    static_cast<std::size_t>(n) * 8;
                auto read_channel = [&](std::vector<double> &dst) -> bool {
                    if (p + 4 > bytes.size()) return false;
                    std::uint32_t chan_size = read_u32(bytes, p);
                    p += 4;
                    if (chan_size != expected) return false;
                    if (p + chan_size > bytes.size()) return false;
                    dst.reserve(n);
                    for (std::int32_t i = 0; i < n; ++i) {
                        dst.push_back(
                            read_f64_unaligned(bytes, p + i * 8));
                    }
                    p += chan_size;
                    return true;
                };
                // 3 position channels
                bool pos_ok =
                    read_channel(d.positions_x) &&
                    read_channel(d.positions_y) &&
                    read_channel(d.positions_z);
                if (pos_ok && p + 4 <= bytes.size()) {
                    // Separator (u32 = 0)
                    std::uint32_t sep = read_u32(bytes, p);
                    p += 4;
                    if (sep == 0) {
                        // 3 normal channels
                        bool nor_ok =
                            read_channel(d.normals_x) &&
                            read_channel(d.normals_y) &&
                            read_channel(d.normals_z);
                        if (nor_ok && p + 4 <= bytes.size()) {
                            // u32 num_uv_channels
                            std::uint32_t num_uv = read_u32(bytes, p);
                            p += 4;
                            if (num_uv <= 32) {
                                for (std::uint32_t ch = 0; ch < num_uv; ++ch) {
                                    if (p + 4 > bytes.size()) break;
                                    std::uint32_t sz = read_u32(bytes, p);
                                    p += 4;
                                    std::vector<double> chan;
                                    if (sz == 0) {
                                        // empty channel
                                    } else if (sz == expected &&
                                               p + sz <= bytes.size()) {
                                        chan.reserve(n);
                                        for (std::int32_t i = 0; i < n; ++i) {
                                            chan.push_back(
                                                read_f64_unaligned(
                                                    bytes, p + i * 8));
                                        }
                                        p += sz;
                                    } else {
                                        // Unexpected channel size - bail out
                                        // without recording further channels.
                                        break;
                                    }
                                    d.uv_channels.push_back(std::move(chan));
                                }
                            }
                        } else if (!nor_ok) {
                            // Don't keep half-decoded normals.
                            d.normals_x.clear();
                            d.normals_y.clear();
                            d.normals_z.clear();
                        }
                    }
                }
                if (!pos_ok) {
                    // Don't keep half-decoded positions.
                    d.positions_x.clear();
                    d.positions_y.clear();
                    d.positions_z.clear();
                }
            }
        }
        out.descriptor = d;
        return true;
    }

    if (class_name == "TriListDescriptor") {
        TriListDescriptor d;
        d.header = hdr;
        // Simple form (95+% of shipped files):
        //   u32 type_code = 1, u32 flag_a = 0, u32 flag_b = 1
        //   per sublist: u32 num_indices, u32 size_bytes, u32 indices[]
        //   between sublists: u32 marker = 1
        if (after_hdr + 12 <= bytes.size() &&
            after_hdr + 12 <= next_token_pos) {
            d.type_code = read_u32(bytes, after_hdr);
            d.flag_a    = read_u32(bytes, after_hdr + 4);
            d.flag_b    = read_u32(bytes, after_hdr + 8);
            std::size_t p = after_hdr + 12;
            const std::size_t body_end = std::min(bytes.size(), next_token_pos);
            std::int32_t total_indices = 0;
            bool ok = true;
            while (p + 8 <= body_end) {
                std::uint32_t num_idx = read_u32(bytes, p);
                std::uint32_t sz_bytes = read_u32(bytes, p + 4);
                if (sz_bytes != num_idx * 4u || p + 8 + sz_bytes > body_end) {
                    ok = false;
                    break;
                }
                TriListSublist sub;
                sub.num_indices = static_cast<std::int32_t>(num_idx);
                sub.indices.reserve(num_idx);
                for (std::uint32_t i = 0; i < num_idx; ++i) {
                    sub.indices.push_back(
                        read_u32(bytes, p + 8 + i * 4));
                }
                p += 8 + sz_bytes;
                total_indices += static_cast<std::int32_t>(num_idx);
                d.sublists.push_back(std::move(sub));
                // Optional marker u32=1 between sublists
                if (p + 4 <= body_end) {
                    if (read_u32(bytes, p) == 1) {
                        p += 4;
                    } else {
                        break;
                    }
                }
            }
            d.num_indices = total_indices;
            d.fields_decoded = ok && !d.sublists.empty();
        }
        out.descriptor = d;
        return true;
    }
    if (class_name == "TriStripDescriptor" ||
        class_name == "TriFanDescriptor") {
        // TriStrip / TriFan body layout (observed):
        //   u32 type_code = 1
        //   per segment (20 bytes): idx_a, idx_b, 1, 0, 1
        // The (1, 0, 1) tail of each segment mirrors a universal-header
        // pattern; its meaning is not pinned but it is constant.
        std::vector<StripSegment> segments;
        std::uint32_t type_code = 0;
        bool fields_ok = false;
        if (after_hdr + 4 <= bytes.size() &&
            after_hdr + 4 <= next_token_pos) {
            type_code = read_u32(bytes, after_hdr);
            std::size_t p = after_hdr + 4;
            const std::size_t body_end =
                std::min(bytes.size(), next_token_pos);
            fields_ok = true;
            while (p + 20 <= body_end) {
                std::uint32_t a = read_u32(bytes, p);
                std::uint32_t b = read_u32(bytes, p + 4);
                std::uint32_t m1 = read_u32(bytes, p + 8);
                std::uint32_t m2 = read_u32(bytes, p + 12);
                std::uint32_t m3 = read_u32(bytes, p + 16);
                if (m1 != 1 || m2 != 0 || m3 != 1) {
                    // Stop at first non-matching segment but keep what we
                    // have; the trailer of the body is variable.
                    break;
                }
                segments.push_back({a, b});
                p += 20;
            }
        }
        if (class_name == "TriStripDescriptor") {
            TriStripDescriptor d;
            d.header = hdr;
            d.type_code = type_code;
            d.segments = std::move(segments);
            d.num_indices = static_cast<std::int32_t>(d.segments.size());
            d.fields_decoded = fields_ok;
            out.descriptor = d;
        } else {
            TriFanDescriptor d;
            d.header = hdr;
            d.type_code = type_code;
            d.segments = std::move(segments);
            d.num_indices = static_cast<std::int32_t>(d.segments.size());
            d.fields_decoded = fields_ok;
            out.descriptor = d;
        }
        return true;
    }

    return false;
}

// Recursive walk: pull the next class-name token at `pos`, decode its
// payload if we can, then for every plausible token in the body region
// (up to the file's end or the next sibling-class boundary determined
// by the parent's known field counts) recurse into a child node.
//
// For un-decoded classes we don't know where the body ends, so we
// collect children using flat scan-ahead and the caller stitches them
// into a sibling chain.
//
// Returns the new file-stream position after this node and its
// recursive children have been consumed.
//
// The body-end determination for known classes uses the payload's
// child-count hint when present (e.g. GroupDescriptor.num_children).
// For everything else we recurse into a SINGLE child node and stop —
// matches the typed-stream's depth-first serialization for most
// hierarchy classes.
std::size_t parse_node(std::span<const std::uint8_t> bytes, std::size_t pos,
                        std::size_t max_pos, ObjectNodePtr &out,
                        std::unordered_map<std::string, std::uint32_t> &counts);

std::size_t parse_children(std::span<const std::uint8_t> bytes, std::size_t pos,
                            std::size_t max_pos, std::size_t expected_count,
                            ObjectNode &parent,
                            std::unordered_map<std::string, std::uint32_t> &counts) {
    for (std::size_t i = 0; i < expected_count && pos < max_pos; ++i) {
        ObjectNodePtr child;
        std::size_t next_tok = find_next_token(bytes, pos);
        if (next_tok >= max_pos) break;
        pos = parse_node(bytes, next_tok, max_pos, child, counts);
        if (child) parent.children.push_back(child);
    }
    return pos;
}

std::size_t parse_node(std::span<const std::uint8_t> bytes, std::size_t pos,
                        std::size_t max_pos, ObjectNodePtr &out,
                        std::unordered_map<std::string, std::uint32_t> &counts) {
    if (pos + 4 > max_pos) return max_pos;
    std::uint32_t n = read_u32(bytes, pos);
    if (!is_plausible_token(bytes, pos, n)) {
        return pos + 1;
    }
    std::string name(reinterpret_cast<const char *>(&bytes[pos + 4]), n - 1);
    if (!is_known_object_class(name) &&
        !(name.size() >= 4 &&
          (name.compare(name.size() - 4, 4, ".mip") == 0 ||
           name.compare(name.size() - 4, 4, ".3do") == 0))) {
        return pos + 1;
    }

    auto node = std::make_shared<ObjectNode>();
    node->class_name  = name;
    node->body_offset = pos + 4 + n;
    counts[name]++;
    out = node;

    std::size_t body_pos = pos + 4 + n;

    // String tokens that aren't *Descriptor (e.g. "series_flagger.mip")
    // have no body of their own.  They are leaves.
    if (!is_known_object_class(name)) {
        return body_pos;
    }

    std::size_t next_tok_for_body = find_next_class_token(bytes, body_pos);
    if (next_tok_for_body > max_pos) next_tok_for_body = max_pos;
    // The 8 bytes immediately before a class-name token are the
    // typed-stream's (parent_id, this_id) pair for the NEXT object —
    // they're NOT part of THIS object's body.  Trim them when bracketing
    // the body for per-class decoders.  Only applies when there is a
    // following class-name token in this scope.
    std::size_t body_end = next_tok_for_body;
    if (body_end >= 8 && body_end <= max_pos && body_end < bytes.size()) {
        // Heuristic: the (parent_id, this_id) pair sits at body_end-8..-4 and -4..0.
        // If both u32s are plausibly small integers, treat them as the
        // typed-stream ID pair and shrink body_end.
        std::uint32_t a = read_u32(bytes, body_end - 8);
        std::uint32_t b = read_u32(bytes, body_end - 4);
        if (a < 0x10000 && b < 0x10000 && b == a + 1) {
            body_end -= 8;
        }
    }
    decode_body(bytes, body_pos, body_end, name, *node);

    // Determine expected child count from payload (best-effort).
    std::size_t expected_children = 0;
    bool        has_child_hint    = false;
    if (auto *g = node->as<GroupPayload>()) {
        expected_children = g->num_children;
        has_child_hint    = true;
    } else if (auto *t = node->as<TrackPayload>()) {
        expected_children = t->num_segments;
        has_child_hint    = true;
    }

    // For known-shape parents we walk exactly that many children.
    // For everything else we recurse into the first reachable token
    // — depth-first chase, but we stop at the next sibling at the
    // outer level (since our caller is in a child-loop).
    if (has_child_hint && expected_children > 0) {
        body_pos = parse_children(bytes, body_pos, max_pos,
                                  expected_children, *node, counts);
    } else {
        // Try to ingest one child (depth-first chase).  Skip class-less
        // gaps using the token scanner.
        std::size_t next_tok = find_next_token(bytes, body_pos);
        if (next_tok < max_pos) {
            // Avoid latching onto an asset-ref string (e.g. ".mip"
            // texture name) as a "child" when this descriptor is a
            // texture-bearer; we still record it but don't recurse.
            std::string peek_name;
            std::uint32_t pn = read_u32(bytes, next_tok);
            if (is_plausible_token(bytes, next_tok, pn)) {
                peek_name.assign(reinterpret_cast<const char *>(
                                     &bytes[next_tok + 4]), pn - 1);
            }
            if (!peek_name.empty() && is_known_object_class(peek_name)) {
                ObjectNodePtr child;
                body_pos = parse_node(bytes, next_tok, max_pos, child, counts);
                if (child) node->children.push_back(child);
            } else {
                body_pos = next_tok;
            }
        }
    }

    node->body_length = body_pos - node->body_offset;
    return body_pos;
}

}  // namespace

const ObjectNode* ObjectTree::find_first(std::string_view name) const {
    if (!root) return nullptr;
    std::vector<const ObjectNode *> stack{ root.get() };
    while (!stack.empty()) {
        auto *n = stack.back();
        stack.pop_back();
        if (n->class_name == name) return n;
        for (auto &c : n->children) stack.push_back(c.get());
    }
    return nullptr;
}

ObjectTree ObjectTree::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 8) {
        throw std::runtime_error("ObjectTree: file too short for 8-byte header");
    }
    ObjectTree t;
    t.stream_version_a = read_u32(bytes, 0);
    t.stream_version_b = read_u32(bytes, 4);

    std::size_t pos = find_next_token(bytes, 8);
    if (pos >= bytes.size()) return t;

    parse_node(bytes, pos, bytes.size(), t.root, t.class_counts);
    return t;
}

}  // namespace opennr
