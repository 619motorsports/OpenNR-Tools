#include "fs/papyrus_descriptors.h"

#include <cstring>
#include <stdexcept>

namespace opennr::papyrus {

// ---- Universal header ----------------------------------------------------

DescriptorHeader read_descriptor_header(Archive& ar) {
    DescriptorHeader h;
    h.version     = ar.read_u32();
    h.name_length = ar.read_u32();
    if (h.name_length > 0) {
        // name_length includes the trailing NUL; read the bytes and drop it.
        std::vector<char> buf(h.name_length);
        ar.read_bytes(buf.data(), h.name_length);
        std::size_t n = h.name_length;
        if (n > 0 && buf[n - 1] == 0) --n;
        h.name.assign(buf.data(), n);
    }
    return h;
}

// ---- TIER FULL implementations ------------------------------------------

// Helper: read a `transferPointer`-style alloc-size prefix and verify it
// matches the expected byte count.  rts.dll's transferPointer /
// transferPointerButNotData both emit a `u32 alloc_size` immediately
// before the payload (when reading) — even when the count is 0, the
// prefix is still on the wire for pointer-array kinds (this matters for
// count == 0 cases, where the prefix is `0`).
static void read_alloc_size_check(Archive& ar, std::uint32_t expected,
                                  const char* where) {
    auto got = ar.read_u32();
    if (got != expected) {
        throw std::runtime_error(
            std::string(where) + ": alloc_size " + std::to_string(got) +
            " != expected " + std::to_string(expected));
    }
}

void TrackDescriptor::read_body(Archive& ar) {
    // Universal NodeDescriptor parent (FUN_005e0300) reads a u32 magic = 1
    // followed by the descriptor name; both consumed by DescriptorBase::read
    // before we get here.  Body proper starts with the per-class version.
    version      = ar.read_u32();
    if (version > 8) {
        throw std::runtime_error(
            "TrackDescriptor: implausible version " + std::to_string(version));
    }
    num_segments = ar.read_i32();
    if (num_segments < 0 || num_segments > (1 << 20)) {
        throw std::runtime_error(
            "TrackDescriptor: implausible num_segments " +
            std::to_string(num_segments));
    }

    if (version >= 3) {
        flag = ar.read_u8();
        if (version == 8) {
            // SMALL branch — the version-8 path reads 3 doubles + 1 u32
            // directly into the struct.  Versions 3..7 follow a different
            // "BIG" branch that reads more doubles and converts them; we
            // don't implement that (shipped tracks are all v8).
            scalar_a = ar.read_f64();
            scalar_b = ar.read_f64();
            scalar_c = ar.read_f64();
            scalar_d_u32 = ar.read_u32();
        } else {
            // Legacy compatibility payload.  The native v3-v7 reader loads
            // these doubles into stack temporaries and leaves the constructor
            // defaults (0.2, 0.2, 0.2, 0) in the modern runtime slots.
            std::size_t count = 5;
            if (version >= 5) count += 8;
            if (version >= 6) count += 8;
            legacy_scalars.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                legacy_scalars.push_back(ar.read_f64());
            }
        }
    }

    if (version >= 7) {
        num_records_e = ar.read_u32();
        if (num_records_e > (1u << 16)) {
            throw std::runtime_error(
                "TrackDescriptor: implausible num_records_e " +
                std::to_string(num_records_e));
        }
        const std::uint32_t e_bytes = num_records_e * 48u;
        // transferPointer alloc_size prefix.
        read_alloc_size_check(ar, e_bytes, "TrackDescriptor.records_e");
        records_e_raw.resize(e_bytes);
        if (e_bytes) ar.read_bytes(records_e_raw.data(), e_bytes);
    }

    if (version == 2 || version > 3) {
        // transferPersistentObject — directly via read_object.
        single_child = ar.read_object();

        auto count_c = ar.read_u32();
        if (count_c > (1u << 20)) {
            throw std::runtime_error(
                "TrackDescriptor: implausible num_children_c " +
                std::to_string(count_c));
        }
        // transferPointerButNotData alloc_size prefix (count_c × 4 bytes
        // for the pointer-array).
        read_alloc_size_check(ar, count_c * 4u, "TrackDescriptor.children_c");
        children_c.reserve(count_c);
        for (std::uint32_t i = 0; i < count_c; ++i) {
            children_c.push_back(ar.read_object());
        }
    }

    auto count_b = ar.read_u32();
    if (count_b > (1u << 20)) {
        throw std::runtime_error(
            "TrackDescriptor: implausible num_children_b " +
            std::to_string(count_b));
    }
    read_alloc_size_check(ar, count_b * 4u, "TrackDescriptor.children_b");
    children_b.reserve(count_b);
    for (std::uint32_t i = 0; i < count_b; ++i) {
        children_b.push_back(ar.read_object());
    }

    // segments[] is also a transferPointerButNotData-prefixed array.  A
    // failed child parse invalidates the object graph: swallowing it here
    // used to make malformed streams look like successfully parsed tracks.
    read_alloc_size_check(
        ar, static_cast<std::uint32_t>(num_segments) * 4u,
        "TrackDescriptor.segments");
    segments.reserve(static_cast<std::size_t>(num_segments));
    for (std::int32_t i = 0; i < num_segments; ++i) {
        segments.push_back(ar.read_object());
    }
    segments_parsed_count = num_segments;

    body_complete = true;
}

void LodSwitchDescriptor::read_body(Archive& ar) {
    flag_a = ar.read_u32();
    flag_b = ar.read_u32();
    ar.read_bytes(bbox, sizeof(bbox));
    magic = ar.read_u32();
    num_lod_levels = ar.read_u32();
    if (num_lod_levels > (1u << 16)) {
        throw std::runtime_error(
            "LodSwitchDescriptor: implausible num_lod_levels " +
            std::to_string(num_lod_levels));
    }
    centre_x = ar.read_f64();
    centre_y = ar.read_f64();
    centre_z = ar.read_f64();
    if (magic > 1) {
        flag = ar.read_u8();
    }
    // transferPointer for switch distances: u32 size + size bytes.
    auto alloc_size_a = ar.read_u32();
    if (alloc_size_a != num_lod_levels * 8) {
        throw std::runtime_error(
            "LodSwitchDescriptor: distances alloc_size " +
            std::to_string(alloc_size_a) + " != num_lod_levels*8 " +
            std::to_string(num_lod_levels * 8));
    }
    switch_distances.resize(num_lod_levels);
    for (std::uint32_t i = 0; i < num_lod_levels; ++i) {
        switch_distances[i] = ar.read_f64();
    }
    // transferPointerButNotData for children pointer array: u32 size only.
    auto alloc_size_b = ar.read_u32();
    if (alloc_size_b != num_lod_levels * 4) {
        throw std::runtime_error(
            "LodSwitchDescriptor: children alloc_size " +
            std::to_string(alloc_size_b) + " != num_lod_levels*4 " +
            std::to_string(num_lod_levels * 4));
    }
    children.reserve(num_lod_levels);
    for (std::uint32_t i = 0; i < num_lod_levels; ++i) {
        children.push_back(ar.read_object());
    }
    body_complete = true;
}

void StateSwitchDescriptor::read_body(Archive& ar) {
    flag_a = ar.read_u32();
    flag_b = ar.read_u32();
    ar.read_bytes(bbox, sizeof(bbox));
    magic = ar.read_u32();
    state_name = ar.read_lp_string();
    if (magic >= 2) {
        flag = ar.read_u8();
    }
    // The runtime forces flag = 1 for these three magic names.  We
    // mirror that here so consumers don't need to re-check.
    if (state_name == "Weather" || state_name == "Weekend" ||
        state_name == "Day_Night") {
        flag = 1;
    }
    num_states = ar.read_u32();
    if (num_states > (1u << 16)) {
        throw std::runtime_error(
            "StateSwitchDescriptor: implausible num_states " +
            std::to_string(num_states));
    }
    default_value = ar.read_f64();
    auto alloc_size_a = ar.read_u32();
    auto expected_a = num_states * 8 + 8;       // (num_states + 1) doubles
    if (alloc_size_a != expected_a) {
        throw std::runtime_error(
            "StateSwitchDescriptor: state_values alloc_size " +
            std::to_string(alloc_size_a) + " != " + std::to_string(expected_a));
    }
    state_values.resize(num_states + 1);
    for (std::uint32_t i = 0; i < num_states + 1; ++i) {
        state_values[i] = ar.read_f64();
    }
    auto alloc_size_b = ar.read_u32();
    if (alloc_size_b != num_states * 4) {
        throw std::runtime_error(
            "StateSwitchDescriptor: children alloc_size " +
            std::to_string(alloc_size_b) + " != num_states*4 " +
            std::to_string(num_states * 4));
    }
    children.reserve(num_states);
    for (std::uint32_t i = 0; i < num_states; ++i) {
        children.push_back(ar.read_object());
    }
    body_complete = true;
}

void ProgressiveModificationDescriptor::read_body(Archive& ar) {
    change_num_vertices   = ar.read_i32();
    num_modified_vertices = ar.read_i32();
    change_num_tris       = ar.read_i32();
    num_modified_tris     = ar.read_i32();
    body_complete = true;
}

void TransformDescriptor::read_body(Archive& ar) {
    // ChildNodeDescriptor parent: u32 magic_node + u32 magic_child + 48 bbox.
    flag_a = ar.read_u32();
    flag_b = ar.read_u32();
    ar.read_bytes(bbox, sizeof(bbox));
    // TransformDescriptor body proper.
    marker = ar.read_u32();
    tx    = ar.read_f64();
    ty    = ar.read_f64();
    tz    = ar.read_f64();
    yaw   = ar.read_f64();
    pitch = ar.read_f64();
    roll  = ar.read_f64();
    // The Transform node always wraps a child (NR2003 FUN_005e07f0
    // unconditionally calls transferPersistentObject(&child)).  In
    // practice this is a Group/Shape/Geometry sub-tree.
    child = ar.read_object();
    body_complete = true;
}

void SegmentDescriptor::read_body(Archive& ar) {
    // The universal NodeDescriptor parent magic is consumed by
    // DescriptorBase::read before this entrypoint.  Body proper starts
    // with the per-class version (default 2, capped < 3 when
    // stream_version == 0 per FUN_005e5b40's bounds check).
    version = ar.read_u32();
    if (version == 0 || version >= 3) {
        throw std::runtime_error(
            "SegmentDescriptor: implausible version " + std::to_string(version));
    }

    auto read_ref_list = [&](std::vector<std::shared_ptr<PersistentObject>>& dst,
                             const char* where) {
        auto count = ar.read_u32();
        if (count > (1u << 20)) {
            throw std::runtime_error(
                std::string("SegmentDescriptor.") + where +
                ": implausible count " + std::to_string(count));
        }
        // FUN_006c38fc is only called when count > 0 — the alloc_size
        // prefix is omitted entirely when the array is empty.
        if (count > 0) {
            read_alloc_size_check(ar, count * 4u,
                ("SegmentDescriptor." + std::string(where)).c_str());
            dst.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                dst.push_back(ar.read_object());
            }
        }
    };
    read_ref_list(x_sections, "x_sections");
    read_ref_list(f_sections, "f_sections");
    read_ref_list(w_sections, "w_sections");

    segment_kind = ar.read_i32();
    if (segment_kind == -1) {
        // Sentinel — body ends here.
        body_complete = true;
        return;
    }

    pos_a   = ar.read_f64();
    pos_b   = ar.read_f64();
    angle_c = ar.read_f64();
    pos_d   = ar.read_f64();
    pos_e   = ar.read_f64();
    angle_f = ar.read_f64();
    flag_a  = ar.read_u8();
    flag_b  = ar.read_u8();

    auto n12 = ar.read_u32();
    if (n12 > (1u << 20)) {
        throw std::runtime_error(
            "SegmentDescriptor: implausible records_12 count " +
            std::to_string(n12));
    }
    if (n12 > 0) {
        // transferPointer alloc-size prefix (count_12 × 12 bytes).
        read_alloc_size_check(ar, n12 * 12u,
            "SegmentDescriptor.records_12");
        records_12_raw.resize(static_cast<std::size_t>(n12) * 12);
        ar.read_bytes(records_12_raw.data(), records_12_raw.size());
    }

    auto n16 = ar.read_u32();
    if (n16 > (1u << 20)) {
        throw std::runtime_error(
            "SegmentDescriptor: implausible records_16 count " +
            std::to_string(n16));
    }
    if (n16 > 0) {
        read_alloc_size_check(ar, n16 * 16u,
            "SegmentDescriptor.records_16");
        records_16_raw.resize(static_cast<std::size_t>(n16) * 16);
        ar.read_bytes(records_16_raw.data(), records_16_raw.size());
    }
    body_complete = true;
}

void TextureDescriptor::read_body(Archive& ar) {
    magic = ar.read_u32();
    texture_name = ar.read_lp_string();
    if (magic >= 2) flag = ar.read_u8();
    body_complete = true;
}

void PlainVertexListDescriptor::read_body(Archive& ar) {
    // Two u32 magics: one from the VertexList parent's read, one from
    // PlainVertexList's own read.  Both observed = 1 on shipped files.
    type_code    = ar.read_u32();   // VertexList parent magic
    flag         = ar.read_u32();   // PVL own magic
    num_vertices = ar.read_i32();
    // FUN_005e11f0 enforces a hard cap of 0x4098 on num_vertices.
    if (num_vertices < 0 || num_vertices > 0x4098) {
        throw std::runtime_error(
            "PlainVertexListDescriptor: implausible num_vertices " +
            std::to_string(num_vertices));
    }
    auto n = static_cast<std::size_t>(num_vertices);

    // Read one transferPointer-style channel: u32 size + size bytes of
    // doubles, where size is either 0 (empty) or n*8 (full).  Used for
    // both the 7 position/normal channels and the 24 UV/attribute channels.
    auto read_channel = [&](std::vector<double>& dst) {
        auto size_bytes = ar.read_u32();
        if (size_bytes == 0) {
            dst.clear();
            return;
        }
        if (size_bytes != n * 8) {
            throw std::runtime_error(
                "PlainVertexListDescriptor: channel size " +
                std::to_string(size_bytes) + " != expected " +
                std::to_string(n * 8));
        }
        dst.resize(n);
        for (std::size_t i = 0; i < n; ++i) dst[i] = ar.read_f64();
    };

    // 7 transferPointer channels per FUN_005e11f0 (NR2003.exe 0x005e11f0):
    //   +0x74 pos_x, +0x78 pos_y, +0x7c pos_z   — vertex positions
    //   +0x80 reserved                          — empty on shipped files
    //   +0x84 norm_x, +0x88 norm_y, +0x8c norm_z — vertex normals
    read_channel(positions_x);
    read_channel(positions_y);
    read_channel(positions_z);
    read_channel(reserved_channel);     // the channel between pos and norm
    read_channel(normals_x);
    read_channel(normals_y);
    read_channel(normals_z);

    // FUN_005e0c40: UV / attribute reader.  Reads a u32 attr_magic
    // (constrained to <= 3 for stream_version==0 files) followed by
    // unconditional 24 transferPointer channels.  Slots 0 and 1 are U
    // and V; the remaining 22 are reserved attribute slots that are
    // empty on shipped files but read all the same.
    attr_magic = ar.read_u32();
    if (attr_magic > 3) {
        throw std::runtime_error(
            "PlainVertexListDescriptor: implausible attr_magic " +
            std::to_string(attr_magic));
    }
    uv_channels.clear();
    uv_channels.resize(kAttributeChannelCount);
    for (int ch = 0; ch < kAttributeChannelCount; ++ch) {
        read_channel(uv_channels[ch]);
    }
    body_complete = true;
}

void GeometryDescriptor::read_body(Archive& ar) {
    magic       = ar.read_u32();
    vertex_list = ar.read_object();
    primitive   = ar.read_object();
    body_complete = true;
}

void GroupDescriptor::read_body(Archive& ar) {
    flag_a = ar.read_u32();
    flag_b = ar.read_u32();
    ar.read_bytes(bbox, sizeof(bbox));            // 48 bytes (presumed bbox)
    magic = ar.read_u32();
    num_children = ar.read_u32();
    if (num_children > (1u << 20)) {
        throw std::runtime_error(
            "GroupDescriptor: implausible num_children " +
            std::to_string(num_children));
    }
    // transferPointerButNotData on the writer side wrote a u32 = num_children * 4
    // here before the child objects; consume and sanity-check it.
    auto alloc_size = ar.read_u32();
    if (alloc_size != num_children * 4) {
        throw std::runtime_error(
            "GroupDescriptor: alloc_size " + std::to_string(alloc_size) +
            " != num_children*4 " + std::to_string(num_children * 4));
    }
    children.reserve(num_children);
    for (std::uint32_t i = 0; i < num_children; ++i) {
        children.push_back(ar.read_object());
    }
    body_complete = true;
}

namespace {
// Shared implementation for TriList / TriStrip / TriFan — same disk
// format, distinguished only by their virtual draw method.
template <class T>
void read_indexed_primitive(T& self, Archive& ar) {
    self.flag_a = ar.read_u32();
    self.next_primitive = ar.read_object();
    self.magic = ar.read_u32();
    self.num_indices = ar.read_u32();
    if (self.num_indices > 0x4098) {
        throw std::runtime_error(
            std::string(T::kClassName) + ": num_indices " +
            std::to_string(self.num_indices) +
            " exceeds runtime cap 0x4098");
    }
    auto alloc_size = ar.read_u32();
    if (alloc_size != self.num_indices * 4) {
        throw std::runtime_error(
            std::string(T::kClassName) + ": alloc_size " +
            std::to_string(alloc_size) + " != num_indices*4 " +
            std::to_string(self.num_indices * 4));
    }
    self.indices.resize(self.num_indices);
    for (std::uint32_t i = 0; i < self.num_indices; ++i) {
        self.indices[i] = ar.read_u32();
    }
    self.body_complete = true;
}
}  // namespace

void TriListDescriptor::read_body(Archive& ar) {
    read_indexed_primitive(*this, ar);
}
void TriStripDescriptor::read_body(Archive& ar) {
    read_indexed_primitive(*this, ar);
}
void TriFanDescriptor::read_body(Archive& ar) {
    read_indexed_primitive(*this, ar);
}

// ---- Helpers for inherited intermediate-class bytes --------------------

namespace {
// NodeDescriptor parent (FUN_005e0360): adds a single u32 magic (=1).
inline std::uint32_t read_node_parent(Archive& ar) {
    return ar.read_u32();
}

// TransformableNode parent (FUN_005e03c0): adds u32 magic + 48 bbox bytes.
inline void read_transformable_parent(
    Archive& ar, std::uint32_t& flag_a, std::uint32_t& flag_b,
    std::uint8_t (&bbox)[48]) {
    flag_a = ar.read_u32();
    flag_b = ar.read_u32();
    ar.read_bytes(bbox, sizeof(bbox));
}
}  // namespace

void ShapeDescriptor::read_body(Archive& ar) {
    magic_node = ar.read_u32();
    magic      = ar.read_u32();
    appearance = ar.read_object();
    geometry   = ar.read_object();
    body_complete = true;
}

void AppearanceDescriptor::read_body(Archive& ar) {
    magic = ar.read_u32();
    // 6 unconditional texture slots @ struct offsets +0xc..+0x20.
    for (int i = 0; i < 6; ++i) texture_slots[i] = ar.read_object();
    // Version-2 appearances store a seventh texture slot.
    if (magic >= 2) {
        texture_slots[6] = ar.read_object();
    }
    ambient_r = ar.read_f64();
    ambient_g = ar.read_f64();
    ambient_b = ar.read_f64();
    diffuse_r = ar.read_f64();
    diffuse_g = ar.read_f64();
    diffuse_b = ar.read_f64();
    specular_r = ar.read_f64();
    specular_g = ar.read_f64();
    specular_b = ar.read_f64();
    shinyness    = ar.read_f64();
    reflectivity = ar.read_f64();
    opacity      = ar.read_f64();
    envmap_index = ar.read_f32();
    body_complete = true;
}

void GroupingNodeDescriptor::read_body(Archive& ar) {
    flag_a = ar.read_u32();
    flag_b = ar.read_u32();
    ar.read_bytes(bbox, sizeof(bbox));
    magic = ar.read_u32();
    num_children = ar.read_u32();
    if (num_children > (1u << 16)) {
        throw std::runtime_error(
            "GroupingNodeDescriptor: implausible num_children " +
            std::to_string(num_children));
    }
    centre_x = ar.read_f64();
    centre_y = ar.read_f64();
    centre_z = ar.read_f64();
    if (magic > 1) flag = ar.read_u8();
    auto alloc_a = ar.read_u32();
    if (alloc_a != num_children * 8) {
        throw std::runtime_error(
            "GroupingNodeDescriptor: distances alloc_size mismatch");
    }
    switch_distances.resize(num_children);
    for (std::uint32_t i = 0; i < num_children; ++i) {
        switch_distances[i] = ar.read_f64();
    }
    auto alloc_b = ar.read_u32();
    if (alloc_b != num_children * 4) {
        throw std::runtime_error(
            "GroupingNodeDescriptor: children alloc_size mismatch");
    }
    children.reserve(num_children);
    for (std::uint32_t i = 0; i < num_children; ++i) {
        children.push_back(ar.read_object());
    }
    body_complete = true;
}

void PointLightDescriptor::read_body(Archive& ar) {
    // NodeDescriptor parent.
    magic_node = ar.read_u32();
    magic      = ar.read_u32();
    if (magic > 3) {
        throw std::runtime_error(
            "PointLightDescriptor: unsupported magic " +
            std::to_string(magic));
    }
    if (magic >= 2) radius_squared = ar.read_f64();
    if (magic < 3) {
        for (double& component : legacy_color) component = ar.read_f64();
        legacy_diffuse_scale = ar.read_f64();
        legacy_ambient_scale = ar.read_f64();
        diffuse_r = static_cast<float>(legacy_color[0] * legacy_diffuse_scale);
        diffuse_g = static_cast<float>(legacy_color[1] * legacy_diffuse_scale);
        diffuse_b = static_cast<float>(legacy_color[2] * legacy_diffuse_scale);
        ambient_r = static_cast<float>(legacy_color[0] * legacy_ambient_scale);
        ambient_g = static_cast<float>(legacy_color[1] * legacy_ambient_scale);
        ambient_b = static_cast<float>(legacy_color[2] * legacy_ambient_scale);
    } else {
        diffuse_r = ar.read_f32();
        diffuse_g = ar.read_f32();
        diffuse_b = ar.read_f32();
        ambient_r = ar.read_f32();
        ambient_g = ar.read_f32();
        ambient_b = ar.read_f32();
    }
    position_x = ar.read_f64();
    position_y = ar.read_f64();
    position_z = ar.read_f64();
    body_complete = true;
}

void AppNodeDescriptor::read_body(Archive& ar) {
    flag_a = ar.read_u32();
    flag_b = ar.read_u32();
    ar.read_bytes(bbox, sizeof(bbox));
    magic = ar.read_u32();
    app_id = ar.read_u32();
    data_size = ar.read_u32();
    if (data_size > (1u << 24)) {
        throw std::runtime_error(
            "AppNodeDescriptor: implausible data_size " +
            std::to_string(data_size));
    }
    data.resize(data_size);
    if (data_size > 0) ar.read_bytes(data.data(), data_size);
    child = ar.read_object();
    body_complete = true;
}

void InfiniteLightDescriptor::read_body(Archive& ar) {
    magic_node = ar.read_u32();
    magic      = ar.read_u32();
    if (magic > 2) {
        throw std::runtime_error(
            "InfiniteLightDescriptor: unsupported magic " +
            std::to_string(magic));
    }
    if (magic < 2) {
        for (double& component : legacy_color) component = ar.read_f64();
        legacy_diffuse_scale = ar.read_f64();
        legacy_ambient_scale = ar.read_f64();
        diffuse_r = static_cast<float>(legacy_color[0] * legacy_diffuse_scale);
        diffuse_g = static_cast<float>(legacy_color[1] * legacy_diffuse_scale);
        diffuse_b = static_cast<float>(legacy_color[2] * legacy_diffuse_scale);
        ambient_r = static_cast<float>(legacy_color[0] * legacy_ambient_scale);
        ambient_g = static_cast<float>(legacy_color[1] * legacy_ambient_scale);
        ambient_b = static_cast<float>(legacy_color[2] * legacy_ambient_scale);
    } else {
        diffuse_r = ar.read_f32();
        diffuse_g = ar.read_f32();
        diffuse_b = ar.read_f32();
        ambient_r = ar.read_f32();
        ambient_g = ar.read_f32();
        ambient_b = ar.read_f32();
    }
    axis_i = ar.read_f64();
    axis_j = ar.read_f64();
    axis_k = ar.read_f64();
    body_complete = true;
}

void PortalDescriptor::read_body(Archive& ar) {
    magic_node = ar.read_u32();
    magic      = ar.read_u32();
    target     = ar.read_object();
    num_indices = ar.read_u32();
    if (num_indices > (1u << 20)) {
        throw std::runtime_error(
            "PortalDescriptor: implausible num_indices " +
            std::to_string(num_indices));
    }
    auto alloc_size = ar.read_u32();
    if (alloc_size != num_indices * 4) {
        throw std::runtime_error(
            "PortalDescriptor: alloc_size " + std::to_string(alloc_size) +
            " != num_indices*4 " + std::to_string(num_indices * 4));
    }
    indices.resize(num_indices);
    for (std::uint32_t i = 0; i < num_indices; ++i) indices[i] = ar.read_u32();
    body_complete = true;
}

void BillboardDescriptor::read_body(Archive& ar) {
    read_transformable_parent(ar, flag_a, flag_b, bbox);
    magic = ar.read_u32();
    pivot_x = ar.read_f64();
    pivot_y = ar.read_f64();
    pivot_z = ar.read_f64();
    axis_x = ar.read_f64();
    axis_y = ar.read_f64();
    axis_z = ar.read_f64();
    child = ar.read_object();
    body_complete = true;
}

void AnimatedTransformDescriptor::read_body(Archive& ar) {
    // Inherits TransformDescriptor's entire body — including its child
    // object — before adding its own keyframe trailer.  NR2003
    // FUN_005e08e0 unconditionally calls FUN_005e07f0 first.
    flag_a = ar.read_u32();
    flag_b = ar.read_u32();
    ar.read_bytes(bbox, sizeof(bbox));
    transform_marker = ar.read_u32();
    tx     = ar.read_f64();
    ty     = ar.read_f64();
    tz     = ar.read_f64();
    // Per docs/formats/3do_descriptor_layouts.md these three are an
    // axis-angle ROTATION AXIS, not Euler angles — see the descriptor
    // class comment.  The matching angle is per-keyframe.
    axis_x = ar.read_f64();
    axis_y = ar.read_f64();
    axis_z = ar.read_f64();
    child  = ar.read_object();
    // AnimatedTransform's own fields:
    magic = ar.read_u32();
    channel_name = ar.read_lp_string();
    num_keyframes = ar.read_u32();
    if (num_keyframes > (1u << 20)) {
        throw std::runtime_error(
            "AnimatedTransformDescriptor: implausible num_keyframes " +
            std::to_string(num_keyframes));
    }
    auto alloc_size = ar.read_u32();
    if (alloc_size != num_keyframes * 32) {
        throw std::runtime_error(
            "AnimatedTransformDescriptor: alloc_size " +
            std::to_string(alloc_size) + " != num_keyframes*32 " +
            std::to_string(num_keyframes * 32));
    }
    keyframes_raw.resize(static_cast<std::size_t>(num_keyframes) * 32);
    if (!keyframes_raw.empty()) {
        ar.read_bytes(keyframes_raw.data(), keyframes_raw.size());
    }
    keyframes.reserve(num_keyframes);
    for (std::uint32_t i = 0; i < num_keyframes; ++i) {
        const auto* record = keyframes_raw.data() + static_cast<std::size_t>(i) * 32;
        AnimationKeyframe key;
        std::memcpy(&key.timestamp, record, 4);
        std::memcpy(key.rotation, record + 4, 16);
        std::memcpy(key.translation, record + 20, 12);
        keyframes.push_back(key);
    }
    body_complete = true;
}


// ---- Newly-FULL implementations promoted from PARTIAL --------------------

namespace {
inline void check_magic(std::uint32_t got, std::uint32_t want, const char* who) {
    if (got != want) {
        throw std::runtime_error(
            std::string(who) + ": magic " + std::to_string(got) +
            " != expected " + std::to_string(want));
    }
}
}  // namespace

void NodeDescriptor::read_body(Archive& ar) {
    magic = ar.read_u32();
    check_magic(magic, 1, "NodeDescriptor");
    body_complete = true;
}

void ChildNodeDescriptor::read_body(Archive& ar) {
    // No concrete instance in stock files; mirroring NodeDescriptor's
    // u32-magic shape so the registry can dispatch if a stream names it.
    magic = ar.read_u32();
    check_magic(magic, 1, "ChildNodeDescriptor");
    body_complete = true;
}

void VertexListDescriptor::read_body(Archive& ar) {
    magic = ar.read_u32();
    check_magic(magic, 1, "VertexListDescriptor");
    body_complete = true;
}

void SelfLightingDescriptor::read_body(Archive& ar) {
    // TransformableNode parent: u32 flag_a (= 1), u32 flag_b (= 1), 48 bbox.
    read_transformable_parent(ar, flag_a, flag_b, bbox);
    magic = ar.read_u32();
    check_magic(magic, 1, "SelfLightingDescriptor");
    diffuse_set = ar.read_u8();
    specular_set = ar.read_u8();
    ambient_set = ar.read_u8();
    diffuse_r = ar.read_f64();
    diffuse_g = ar.read_f64();
    diffuse_b = ar.read_f64();
    specular_r = ar.read_f64();
    specular_g = ar.read_f64();
    specular_b = ar.read_f64();
    ambient_r = ar.read_f64();
    ambient_g = ar.read_f64();
    ambient_b = ar.read_f64();
    child = ar.read_object();
    body_complete = true;
}

void BiCubicPatchDescriptor::read_body(Archive& ar) {
    // PrimitiveDescriptor parent: u32 magic_prim + inline child object.
    magic_prim = ar.read_u32();
    check_magic(magic_prim, 1, "BiCubicPatchDescriptor::primitive_parent");
    child = ar.read_object();
    // Own body.
    magic = ar.read_u32();
    check_magic(magic, 1, "BiCubicPatchDescriptor");
    for (float& value : scalar_grid) value = ar.read_f32();
    for (double& value : parameters) value = ar.read_f64();
    for (auto& vector : vectors)
        for (float& value : vector) value = ar.read_f32();
    body_complete = true;
}

void ProgressiveMeshDescriptor::read_body(Archive& ar) {
    magic_node = ar.read_u32();
    check_magic(magic_node, 1, "ProgressiveMeshDescriptor::node_parent");
    magic = ar.read_u32();
    check_magic(magic, 1, "ProgressiveMeshDescriptor");
    child_a = ar.read_object();
    child_b = ar.read_object();
    child_c = ar.read_object();
    base_num_vertices = ar.read_u32();
    base_num_tris     = ar.read_u32();
    num_modifications = ar.read_u32();
    if (num_modifications > (1u << 20)) {
        throw std::runtime_error(
            "ProgressiveMeshDescriptor: implausible num_modifications " +
            std::to_string(num_modifications));
    }
    auto alloc_size = ar.read_u32();
    if (alloc_size != num_modifications * 4) {
        throw std::runtime_error(
            "ProgressiveMeshDescriptor: alloc_size " +
            std::to_string(alloc_size) + " != num_modifications*4 " +
            std::to_string(num_modifications * 4));
    }
    modifications.reserve(num_modifications);
    for (std::uint32_t i = 0; i < num_modifications; ++i) {
        modifications.push_back(ar.read_object());
    }
    body_complete = true;
}

void TrackDetailDescriptor::read_body(Archive& ar) {
    magic = ar.read_u32();
    // The runtime check is `magic != 0 -> reject when reading`; we don't
    // enforce that here because the on-disk magic isn't a strict format
    // version (it's used as a runtime scratch slot per the decompile).
    child = ar.read_object();
    f_a = ar.read_f64();
    f_b = ar.read_f64();
    f_c = ar.read_f64();
    f_d = ar.read_f64();
    f_e = ar.read_f64();
    // Two transferPointer calls with a fixed 32-byte payload each.
    auto sa = ar.read_u32();
    if (sa != 32) {
        throw std::runtime_error(
            "TrackDetailDescriptor: buf32a size " + std::to_string(sa) +
            " != 32");
    }
    ar.read_bytes(buf32a, 32);
    auto sb = ar.read_u32();
    if (sb != 32) {
        throw std::runtime_error(
            "TrackDetailDescriptor: buf32b size " + std::to_string(sb) +
            " != 32");
    }
    ar.read_bytes(buf32b, 32);
    body_complete = true;
}

void TSODescriptor::read_body(Archive& ar) {
    magic = ar.read_u32();
    if (magic > 3) {
        throw std::runtime_error(
            "TSODescriptor: magic " + std::to_string(magic) + " > 3");
    }
    name_a = ar.read_lp_string();
    if (magic >= 2) name_b = ar.read_lp_string();
    if (magic >= 3) flag = ar.read_u8();
    body_complete = true;
}

void TSOReferenceDescriptor::read_body(Archive& ar) {
    // Body from NR2003.exe FUN_005e8050 (TSOReferenceDescriptor::read):
    //   universal_header (consumed by DescriptorBase)
    //   u32 magic                                      (cap <= 6, default 6)
    //   6 × f64 transform components                   @ +0xc..+0x34
    //   transferPersistentObject child                 @ +0x50  (the TSO)
    //
    //   magic switch (stream_version == 0 path):
    //     magic == 1: zero defaults, JUMP to name section
    //     magic == 2: zero defaults, clear has_extra high-byte flag
    //     magic == 3: zero +0x40, fall through to read +0x40
    //     magic ∈ {4,5,6}: read u32 conditional_flag    @ +0x40
    //
    //   u8 padding_byte                                  (always)
    //   44 bytes raw inline_payload                      (32 + 4 + 4 + 4;
    //                                                     always read on shipped
    //                                                     files — the allocator
    //                                                     at FUN_005e8020 runs
    //                                                     deterministically and
    //                                                     hands the inline-payload
    //                                                     a buffer to write into)
    //
    //   [if has_extra (magic != 2)]:
    //     u32 extra_count                                @ +0x48
    //     [if extra_count > 0]: extra_count × 0x28 bytes @ +0x44
    //
    //   [if magic >= 6]: lp_string name                  @ +0x4c
    magic = ar.read_u32();
    if (magic > 6) {
        throw std::runtime_error(
            "TSOReferenceDescriptor: magic " + std::to_string(magic) + " > 6");
    }
    for (int i = 0; i < 6; ++i) xform[i] = ar.read_f64();
    child = ar.read_object();

    // Magic == 1 short-circuits to the name read with everything zeroed.
    if (magic == 1) {
        if (magic >= 6) name = ar.read_lp_string();   // unreachable but explicit
        body_complete = true;
        return;
    }

    // For magic in {2,3,4,5,6} (stream_version == 0): the flag/payload
    // gating differs by magic but all of the bytes ARE consumed on disk
    // in shipped files.
    bool read_conditional_flag = (magic >= 4);
    bool has_extra_flag        = (magic != 2);

    if (read_conditional_flag) {
        conditional_flag = ar.read_u32();
    } else {
        conditional_flag = 0;
    }

    // u8 skip_payload flag — controls whether the 44-byte inline payload
    // appears on disk:
    //   skip_payload == 0  →  payload IS present (path B in the disasm:
    //                         FUN_005e8020 allocates a heap buffer at
    //                         this+0x3c, then the 32+4+4+4 = 44 bytes
    //                         are read into it)
    //   skip_payload != 0  →  payload is absent UNLESS magic < 5 (path A)
    //
    // Ghidra named the gate variable `unaff_BP` (Ghidra-confused alias
    // for the u8 we just read into `stack0xffffffc8`) — the equality
    // check `unaff_BP == '\0'` is really `*stack0xffffffc8 == 0`.
    has_extra = (ar.read_u8() != 0);

    const bool read_payload = (magic < 5) || !has_extra;
    if (read_payload) {
        inline_payload.resize(44);
        ar.read_bytes(inline_payload.data(), 44);
    }

    if (has_extra_flag) {
        auto extra_count = ar.read_u32();
        if (extra_count > (1u << 16)) {
            throw std::runtime_error(
                "TSOReferenceDescriptor: implausible extra_count " +
                std::to_string(extra_count));
        }
        if (extra_count > 0) {
            extra_records.resize(std::size_t{extra_count} * 0x28);
            ar.read_bytes(extra_records.data(), extra_records.size());
        }
    }

    if (magic >= 6) name = ar.read_lp_string();
    body_complete = true;
}

void TextureCoordsDescriptor::read_body(Archive& ar) {
    magic = ar.read_u32();
    check_magic(magic, 1, "TextureCoordsDescriptor");
    num_vertices = ar.read_u32();
    if (num_vertices > 0x4098) {
        throw std::runtime_error(
            "TextureCoordsDescriptor: num_vertices " +
            std::to_string(num_vertices) + " exceeds cap 0x4098");
    }
    // Vertex-list trailer (FUN_005e0c40):  u32 magic2 (== 3) then a fixed
    // set of channels, each a transferPointer(num_vertices*8).
    magic2 = ar.read_u32();
    if (magic2 > 3) {
        throw std::runtime_error(
            "TextureCoordsDescriptor: trailer magic " +
            std::to_string(magic2) + " > 3");
    }
    auto read_channel = [&](std::vector<double>& dst) {
        auto sz = ar.read_u32();
        // size == 0 marks the channel as empty (the on-disk
        // transferPointer write side emits size=0 for the channels that
        // aren't populated; e.g. UV-only files leave the 22 attribute
        // slots empty).  Matches PlainVertexListDescriptor's identical
        // FUN_005e0c40 trailer reader.
        if (sz == 0) {
            dst.clear();
            return;
        }
        if (sz != num_vertices * 8) {
            throw std::runtime_error(
                "TextureCoordsDescriptor: channel size " + std::to_string(sz) +
                " != num_vertices*8 " + std::to_string(num_vertices * 8));
        }
        dst.resize(num_vertices);
        for (std::uint32_t i = 0; i < num_vertices; ++i) {
            dst[i] = ar.read_f64();
        }
    };
    // Per FUN_005e0c40, the channel layout is:
    //   [0..3] always read
    //   [4..7] only when magic2 > 2  (gated by the conditional branch)
    //   [8..23] always read (16 channels)
    // Keep the runtime's 24 logical slots even for old streams where 4..7
    // were omitted; compacting 8..23 into those slots changes UV-set identity.
    channels.resize(24);
    for (std::size_t i = 0; i < 4; ++i) read_channel(channels[i]);
    if (magic2 > 2)
        for (std::size_t i = 4; i < 8; ++i) read_channel(channels[i]);
    for (std::size_t i = 8; i < 24; ++i) read_channel(channels[i]);
    body_complete = true;
}

void TrackGrooveDescriptor::read_body(Archive& ar) {
    magic = ar.read_u32();
    // magic == 0 observed; the runtime check is `magic != 0 -> fail` only
    // when *(Archive+5) != 0 (write mode).  Read-only: we accept any.
    num_samples = ar.read_u32();
    if (num_samples > (1u << 20)) {
        throw std::runtime_error(
            "TrackGrooveDescriptor: implausible num_samples " +
            std::to_string(num_samples));
    }
    child  = ar.read_object();
    scalar = ar.read_f64();
    for (int c = 0; c < 5; ++c) {
        auto sz = ar.read_u32();
        if (sz != num_samples * 8) {
            throw std::runtime_error(
                "TrackGrooveDescriptor: channel " + std::to_string(c) +
                " size " + std::to_string(sz) + " != num_samples*8 " +
                std::to_string(num_samples * 8));
        }
        channels[c].resize(num_samples);
        for (std::uint32_t i = 0; i < num_samples; ++i) {
            channels[c][i] = ar.read_f64();
        }
    }
    body_complete = true;
}

void TrackRaceLineDescriptor::read_body(Archive& ar) {
    magic = ar.read_u32();
    num_samples = ar.read_u32();
    if (num_samples > (1u << 20)) {
        throw std::runtime_error(
            "TrackRaceLineDescriptor: implausible num_samples " +
            std::to_string(num_samples));
    }
    child = ar.read_object();
    scalars[0] = ar.read_f64();
    scalars[1] = ar.read_f64();
    scalars[2] = ar.read_f64();
    for (int c = 0; c < 2; ++c) {
        auto sz = ar.read_u32();
        if (sz != num_samples * 8) {
            throw std::runtime_error(
                "TrackRaceLineDescriptor: channel " + std::to_string(c) +
                " size " + std::to_string(sz) + " != num_samples*8 " +
                std::to_string(num_samples * 8));
        }
        channels[c].resize(num_samples);
        for (std::uint32_t i = 0; i < num_samples; ++i) {
            channels[c][i] = ar.read_f64();
        }
    }
    body_complete = true;
}


// ---- Helpers for the second-pass FULL implementations --------------------

namespace {
// Read a single transferPointer(channel, n*elem) pair: u32 size + data.
//
// Matches `Archive::transferPointer` in rts.dll (@ 0x10001ea0): the
// on-disk alloc_size is the source of truth.  alloc_size == 0 is a
// valid "absent channel" sentinel and is accepted everywhere — the
// caller resolves what an empty channel means (typically: leave the
// dst vector empty).  A nonzero value must equal the caller's
// expected size.
//
// Returns the on-disk alloc_size (0 or `expected`); callers branch on
// this to decide whether data follows.
inline std::uint32_t read_alloc_size_eq(Archive& ar, std::uint32_t expected,
                                        const char* who) {
    auto sz = ar.read_u32();
    if (sz != 0 && sz != expected) {
        throw std::runtime_error(
            std::string(who) + ": alloc_size " + std::to_string(sz) +
            " != 0 or expected " + std::to_string(expected));
    }
    return sz;
}

// Read u32 alloc_size + that many bytes as doubles (must be a multiple of 8).
// `alloc_size == 0` leaves `dst` empty (= channel absent on disk).
inline void read_double_channel(Archive& ar, std::vector<double>& dst,
                                std::uint32_t expected_doubles,
                                const char* who) {
    auto sz = read_alloc_size_eq(ar, expected_doubles * 8, who);
    if (sz == 0) { dst.clear(); return; }
    dst.resize(expected_doubles);
    for (std::uint32_t i = 0; i < expected_doubles; ++i) {
        dst[i] = ar.read_f64();
    }
}

// Read u32 alloc_size + that many u32s.  `alloc_size == 0` leaves
// `dst` empty.
inline void read_u32_channel(Archive& ar, std::vector<std::uint32_t>& dst,
                             std::uint32_t expected_u32s,
                             const char* who) {
    auto sz = read_alloc_size_eq(ar, expected_u32s * 4, who);
    if (sz == 0) { dst.clear(); return; }
    dst.resize(expected_u32s);
    for (std::uint32_t i = 0; i < expected_u32s; ++i) {
        dst[i] = ar.read_u32();
    }
}

// transferPointerButNotData(this+off, n*4) — reads ONLY the u32 alloc_size.
// Accepts 0 or expected.
inline void read_alloc_only(Archive& ar, std::uint32_t expected,
                            const char* who) {
    read_alloc_size_eq(ar, expected, who);
}

// Read the post-vertex-list trailer (FUN_005e0c40 in NR2003).
// Outputs the trailer magic + the channel data.  When num_vertices == 0,
// only the magic and per-channel alloc_size (= 0) are consumed.
//
// Channels (per disasm):
//   [0..4): always
//   [4..8): only when magic > 2 in read mode
//   [8..24): always         (16 more channels)
// Total = 20 when magic <= 2, 24 when magic == 3.
void read_pvl_trailer(Archive& ar, std::uint32_t num_vertices,
                       std::uint32_t& out_magic,
                       std::vector<std::vector<double>>& out_channels) {
    out_magic = ar.read_u32();
    if (out_magic > 3) {
        throw std::runtime_error(
            "pvl trailer: magic " + std::to_string(out_magic) + " > 3");
    }
    const std::size_t n_total = (out_magic > 2) ? 24 : 20;
    out_channels.resize(n_total);
    for (std::size_t c = 0; c < n_total; ++c) {
        read_double_channel(ar, out_channels[c], num_vertices,
                            "pvl_trailer_channel");
    }
}
}  // namespace

void MorphVertexListDescriptor::read_body(Archive& ar) {
    magic_parent = ar.read_u32();
    check_magic(magic_parent, 1, "MorphVertexList::vl_parent");
    magic = ar.read_u32();
    check_magic(magic, 2, "MorphVertexList");
    max_vertices = ar.read_u32();
    if (max_vertices > 0x4098) {
        throw std::runtime_error(
            "MorphVertexList: max_vertices " + std::to_string(max_vertices) +
            " exceeds cap 0x4098");
    }
    num_frames = ar.read_u32();
    if (num_frames > (1u << 20)) {
        throw std::runtime_error(
            "MorphVertexList: implausible num_frames " +
            std::to_string(num_frames));
    }

    channels.assign(kTotalChannels, {});
    int flat = 0;
    for (int g = 0; g < kNumGroups; ++g) {
        const int gsize = kChannelsPerGroup[g];
        // 1) Per-channel pointer-array alloc sizes (transferPointerButNotData).
        for (int c = 0; c < gsize; ++c) {
            read_alloc_only(ar, num_frames * 4, "MorphVertexList::group_alloc");
        }
        // 2) Per-frame per-channel data.
        for (int c = 0; c < gsize; ++c) channels[flat + c].resize(num_frames);
        for (std::uint32_t i = 0; i < num_frames; ++i) {
            for (int c = 0; c < gsize; ++c) {
                read_double_channel(ar, channels[flat + c][i], max_vertices,
                                    "MorphVertexList::channel");
            }
        }
        flat += gsize;
    }
    body_complete = true;
}

void LodMorphVertexListDescriptor::read_body(Archive& ar) {
    MorphVertexListDescriptor::read_body(ar);
    lod_magic = ar.read_u32();
    check_magic(lod_magic, 1, "LodMorphVertexList");
    read_double_channel(ar, lod_values, num_frames, "LodMorphVertexList::values");
    body_complete = true;
}

void StateMorphVertexListDescriptor::read_body(Archive& ar) {
    MorphVertexListDescriptor::read_body(ar);
    state_magic = ar.read_u32();
    check_magic(state_magic, 1, "StateMorphVertexList");
    read_double_channel(ar, state_values, num_frames, "StateMorphVertexList::values");
    body_complete = true;
}

void RegionMorphVertexListDescriptor::read_body(Archive& ar) {
    magic_parent = ar.read_u32();
    check_magic(magic_parent, 1, "RegionMorphVertexList::vl_parent");
    magic = ar.read_u32();
    check_magic(magic, 1, "RegionMorphVertexList");
    num_vertices = ar.read_u32();
    if (num_vertices > 0x4098) {
        throw std::runtime_error(
            "RegionMorphVertexList: num_vertices " +
            std::to_string(num_vertices) + " exceeds cap 0x4098");
    }
    for (int i = 0; i < 8; ++i) {
        read_double_channel(ar, channels[i], num_vertices,
                            "RegionMorphVertexList::channel");
    }
    read_pvl_trailer(ar, num_vertices, trailer_magic, trailer_channels);

    num_regions = ar.read_u32();
    if (num_regions > (1u << 20)) {
        throw std::runtime_error(
            "RegionMorphVertexList: implausible num_regions " +
            std::to_string(num_regions));
    }
    regions.resize(num_regions);
    for (std::uint32_t r = 0; r < num_regions; ++r) {
        auto& rec = regions[r];
        rec.name = ar.read_lp_string();
        rec.a = ar.read_u32();
        rec.b = ar.read_u32();
        rec.num_verts_in_region = ar.read_u32();
        for (int c = 0; c < 8; ++c) {
            read_double_channel(ar, rec.channels[c], rec.num_verts_in_region,
                                "RegionMorphRecord::channel");
        }
        read_u32_channel(ar, rec.indices, rec.num_verts_in_region,
                         "RegionMorphRecord::indices");
    }
    body_complete = true;
}

void SpanningVertexListDescriptor::read_body(Archive& ar) {
    magic_parent = ar.read_u32();
    check_magic(magic_parent, 1, "SpanningVertexList::vl_parent");
    magic = ar.read_u32();
    check_magic(magic, 1, "SpanningVertexList");
    max_vertices = ar.read_u32();
    if (max_vertices > 0x4098) {
        throw std::runtime_error(
            "SpanningVertexList: max_vertices " +
            std::to_string(max_vertices) + " exceeds cap 0x4098");
    }
    num_objects = ar.read_u32();
    if (num_objects > (1u << 20)) {
        throw std::runtime_error(
            "SpanningVertexList: implausible num_objects " +
            std::to_string(num_objects));
    }
    // Object array.
    read_alloc_only(ar, num_objects * 4, "SpanningVertexList::object_alloc");
    objects.reserve(num_objects);
    for (std::uint32_t i = 0; i < num_objects; ++i) {
        objects.push_back(ar.read_object());
    }
    // Channel A (single channel × num_objects).
    read_alloc_only(ar, num_objects * 4, "SpanningVertexList::chanA_alloc");
    channels_a.resize(num_objects);
    for (std::uint32_t i = 0; i < num_objects; ++i) {
        read_double_channel(ar, channels_a[i], max_vertices,
                            "SpanningVertexList::chanA");
    }
    // Group B (4 channels × num_objects).
    for (int c = 0; c < 4; ++c) {
        read_alloc_only(ar, num_objects * 4, "SpanningVertexList::chanB_alloc");
        channels_b[c].resize(num_objects);
    }
    for (std::uint32_t i = 0; i < num_objects; ++i) {
        for (int c = 0; c < 4; ++c) {
            read_double_channel(ar, channels_b[c][i], max_vertices,
                                "SpanningVertexList::chanB");
        }
    }
    // Group C (3 channels × num_objects).
    for (int c = 0; c < 3; ++c) {
        read_alloc_only(ar, num_objects * 4, "SpanningVertexList::chanC_alloc");
        channels_c[c].resize(num_objects);
    }
    for (std::uint32_t i = 0; i < num_objects; ++i) {
        for (int c = 0; c < 3; ++c) {
            read_double_channel(ar, channels_c[c][i], max_vertices,
                                "SpanningVertexList::chanC");
        }
    }
    // Trailer (over max_vertices, not num_objects — vertex-list trailer).
    read_pvl_trailer(ar, max_vertices, trailer_magic, trailer_channels);
    body_complete = true;
}

void X_SectionDescriptor::read_body(Archive& ar) {
    magic = ar.read_u32();
    if (magic > 3) {
        throw std::runtime_error(
            "X_SectionDescriptor: magic " + std::to_string(magic) + " > 3");
    }
    // Versions before 3 stored the float-valued coordinates as f64 and the
    // native reader narrowed them after transfer.  Version 3 writes f32.
    auto read_versioned_float = [&]() -> float {
        return magic < 3 ? static_cast<float>(ar.read_f64()) : ar.read_f32();
    };
    lateral_start = read_versioned_float();
    height_start = ar.read_f64();
    slope_start = read_versioned_float();
    lateral_end = read_versioned_float();
    height_end = ar.read_f64();

    // The "sigma" bound check at +0x25: the runtime rejects when
    // height_end > bound, but accepts when <= bound. Since we don't know
    // the bound numerically, we always take the accept path and assume
    // the file is well-formed.  This matches how runtime-loaded files
    // behave (the bound is intended for write-time sanity).
    complete_payload_read = true;
    slope_end = read_versioned_float();

    // The constructor supplies zero for fields absent from older streams.
    if (magic >= 2) visual_curve_mode = ar.read_u8();

    // v >= 3: read start/end seam topology. Older streams end immediately
    // after visual_curve_mode/defaults.
    if (magic < 3) {
        body_complete = true;
        return;
    }
    start_seam.kind = ar.read_u8();
    if (start_seam.kind != 0) {
        start_seam.parameter0 = ar.read_f32();
        start_seam.parameter1 = ar.read_f32();
    }
    if (start_seam.kind == 2) {
        start_seam.source_boundary_index = ar.read_u8();
        start_seam.target_boundary_index = ar.read_u8();
    }
    end_seam.kind = ar.read_u8();
    if (end_seam.kind != 0) {
        end_seam.parameter0 = ar.read_f32();
        end_seam.parameter1 = ar.read_f32();
    }
    if (end_seam.kind == 2) {
        end_seam.source_boundary_index = ar.read_u8();
        end_seam.target_boundary_index = ar.read_u8();
    }
    body_complete = true;
}

namespace {

// Helper: read the F-section per-slot trailer block written by NR2003.exe
// FUN_005e69a0(arc, slot, x_field_value).  Slot ∈ [0..5].
//
//   if x_field_value == 0:
//     0 bytes (block absent)
//   else:
//     u8 gate
//     if gate == 0:
//       2 × u8                    (12 extra-u8 bytes total over 6 slots,
//                                  stored at F+0x2b..+0x36)
//       2 × f64                   (pair_f64, F+0x37..+0x96, stride 0x10)
//       3 × f64                   (triple_f64, F+0x97..+0x126, stride 0x18)
inline void read_fsection_trailer_block(
    Archive& ar, F_SectionDescriptor::TrailerBlock& blk,
    std::uint32_t x_field_value) {
    if (x_field_value == 0) return;
    blk.x_field_nonzero = true;
    blk.gate = ar.read_u8();
    if (blk.gate != 0) return;
    blk.full = true;
    for (int i = 0; i < 2; ++i) {
        blk.flags_u8[i]  = ar.read_u8();
        blk.pair_f64[i]  = ar.read_f64();
    }
    for (int i = 0; i < 3; ++i) {
        blk.triple_f64[i] = ar.read_f64();
    }
}

// Reconstruct the u32 read at byte-offset `off` of an X_SectionDescriptor's
// in-memory struct layout (NR2003's struct, not our parser's).  Only the
// 6 offsets the F-section trailer cares about are wired here.
inline std::uint32_t x_struct_u32_at(
    const X_SectionDescriptor& xs, int off) {
    auto f32_bits = [](float f) -> std::uint32_t {
        std::uint32_t u; std::memcpy(&u, &f, 4); return u;
    };
    auto f64_bits = [](double f) -> std::uint64_t {
        std::uint64_t u; std::memcpy(&u, &f, 8); return u;
    };
    const std::uint64_t f15 = f64_bits(xs.height_start);
    const std::uint32_t f1d = f32_bits(xs.slope_start);
    switch (off) {
        case 0x0c:
            return f32_bits(xs.lateral_start);
        case 0x10:
            return f32_bits(xs.lateral_end);
        case 0x14:
            // visual_curve_mode (1 byte) + low 3 bytes of height_start
            return std::uint32_t(xs.visual_curve_mode)
                 | (std::uint32_t(f15 & 0xFFFFFF) << 8);
        case 0x18:
            // bytes 3..6 of height_start
            return std::uint32_t((f15 >> 24) & 0xFFFFFFFFu);
        case 0x1c:
            // byte 7 of height_start + bytes 0..2 of slope_start
            return std::uint32_t((f15 >> 56) & 0xFF)
                 | ((f1d & 0xFFFFFFu) << 8);
        case 0x20:
            // byte 3 of slope_start (the 3 following bytes are unset
            // by our parser; NR2003 ctor zeroes them, so the high 24
            // bits stay 0).
            return (f1d >> 24) & 0xFFu;
    }
    return 0;
}

}  // namespace

void F_SectionDescriptor::read_body(Archive& ar) {
    magic = ar.read_u32();
    if (magic > 5) {
        throw std::runtime_error(
            "F_SectionDescriptor: magic " + std::to_string(magic) + " > 5");
    }
    // Pre-v5 streams stored these values as f64 and narrowed them on load.
    auto read_versioned_float = [&]() -> float {
        return magic < 5 ? static_cast<float>(ar.read_f64()) : ar.read_f32();
    };
    lateral_start = read_versioned_float();
    lateral_end = read_versioned_float();
    // Constructor defaults used by the native reader for absent fields.
    if (magic >= 3) boundary_mode = ar.read_u8();
    query_tag = magic >= 4 ? ar.read_u32() : 1u;
    surface_code = ar.read_u32();
    // X_Section reference (transferPersistentObject).
    x_section = ar.read_object();

    // The W_Section + 6-slot trailer is gated on the `x_section` object
    // being "non-empty" — at NR2003 runtime that's a 5-u32 check at
    // struct offsets +0xc / +0x10 / +0x14 / +0x18 / +0x20 of WHATEVER
    // class lives at x_section.  In shipped atlanta.ptf the x_section
    // field is sometimes an AppearanceDescriptor (whose first slots are
    // in-memory texture_slot pointers — non-null when the appearance
    // references a Texture), not an X_SectionDescriptor.  Generalize:
    auto x_slot_u32 = [&](int off) -> std::uint32_t {
        if (auto* xs = dynamic_cast<X_SectionDescriptor*>(x_section.get())) {
            return x_struct_u32_at(*xs, off);
        }
        if (auto* ap = dynamic_cast<AppearanceDescriptor*>(x_section.get())) {
            // The bytes at AppearanceDescriptor +0xc / +0x10 / +0x14 /
            // +0x18 / +0x1c / +0x20 are the texture_slot pointers at
            // runtime.  Map a non-null shared_ptr to a non-zero u32.
            const int slot_idx[] = {0, 1, 2, 3, 4, 5};   // offsets +0xc..+0x20
            int idx = (off - 0xc) / 4;
            if (idx >= 0 && idx < 6) {
                return ap->texture_slots[slot_idx[idx]] != nullptr ? 1u : 0u;
            }
            return 0;
        }
        // Any other polymorphic class: assume non-empty for the
        // gate-byte check (so the trailer at least gets a 1-byte gate
        // per slot, which then short-circuits if the gate is non-zero).
        return x_section != nullptr ? 1u : 0u;
    };
    bool x_non_empty = x_slot_u32(0x0c) != 0
                    || x_slot_u32(0x10) != 0
                    || x_slot_u32(0x14) != 0
                    || x_slot_u32(0x18) != 0
                    || x_slot_u32(0x20) != 0;

    if (x_non_empty) {
        // Read W_Section reference (any registered class — typically a
        // W_SectionDescriptor but the slot is polymorphic).
        w_section = ar.read_object();

        // Now the 6-slot trailer.  Slot → X-field offset mapping (call
        // order in FUN_005e6a50): {0xc, 0x10, 0x18, 0x14, 0x1c, 0x20}.
        const int slot_offsets[6] = { 0x0c, 0x10, 0x18, 0x14, 0x1c, 0x20 };
        for (int slot = 0; slot < 6; ++slot) {
            read_fsection_trailer_block(ar, trailer_blocks[slot],
                                        x_slot_u32(slot_offsets[slot]));
        }
    }
    body_complete = true;
}

void W_SectionDescriptor::read_body(Archive& ar) {
    magic = ar.read_u32();
    if (magic > 7) {
        throw std::runtime_error(
            "W_SectionDescriptor: magic " + std::to_string(magic) + " > 7");
    }
    // Pre-v6 streams stored these values as f64 and narrowed them on load.
    auto read_versioned_float = [&]() -> float {
        return magic < 6 ? static_cast<float>(ar.read_f64()) : ar.read_f32();
    };
    lateral_start = read_versioned_float();
    lateral_end = read_versioned_float();
    // Constructor defaults used by the native reader for absent fields.
    if (magic >= 3) boundary_mode = ar.read_u8();
    query_tag = magic >= 4 ? ar.read_u32() : 1u;
    height_start = ar.read_f64();
    height_end = ar.read_f64();
    visual_face_offset_start = ar.read_f64();
    visual_face_offset_end = ar.read_f64();
    collision_half_thickness_start = ar.read_f64();
    collision_half_thickness_end = ar.read_f64();
    height_offset_mode = ar.read_i32();
    wall_profile_kind = magic >= 7 ? ar.read_u32() : 0u;
    if (wall_profile_kind == 1) {
        cubic_profile.resize(0x28);
        ar.read_bytes(cubic_profile.data(), 0x28);
    }
    if (height_offset_mode == -1) {
        throw std::runtime_error("W_SectionDescriptor: height_offset_mode == -1 (invalid)");
    }
    surface_code = ar.read_u32();
    longitudinal_record_count = magic >= 5 ? ar.read_u32() : 1u;
    if (longitudinal_record_count > (1u << 16)) {
        throw std::runtime_error(
            "W_SectionDescriptor: implausible record count " +
            std::to_string(longitudinal_record_count));
    }
    records.resize(longitudinal_record_count);

    // Helper to compute the same "x_field u32 at offset" as F_Section
    // does, but for the per-inner-pair child_a which is also polymorphic.
    auto child_a_u32_at = [&](std::shared_ptr<PersistentObject> obj, int off) -> std::uint32_t {
        if (auto* xs = dynamic_cast<X_SectionDescriptor*>(obj.get())) {
            return x_struct_u32_at(*xs, off);
        }
        if (auto* ap = dynamic_cast<AppearanceDescriptor*>(obj.get())) {
            int idx = (off - 0xc) / 4;
            if (idx >= 0 && idx < 6) {
                return ap->texture_slots[idx] != nullptr ? 1u : 0u;
            }
            return 0;
        }
        return obj ? 1u : 0u;
    };

    // Helper to read a single FUN_005e71f0 block per inner-pair slot.
    // Each block: 1 gate byte; if gate == 0, 2 × (1 byte + 8 bytes f64 +
    // 8 bytes f64) = 34 more bytes.
    auto read_wsection_trailer_block = [&]
        (W_SectionDescriptor::NestedInterpolationBlock& block,
         std::uint32_t x_field_value) {
        if (x_field_value == 0) return;
        block.source_field_nonzero = true;
        block.gate = ar.read_u8();
        if (block.gate != 0) return;
        block.full = true;
        for (int k = 0; k < 2; ++k) {
            block.entries[k].flag = ar.read_u8();
            block.entries[k].rate = ar.read_f64();
            block.entries[k].offset = ar.read_f64();
        }
    };

    for (std::uint32_t r = 0; r < longitudinal_record_count; ++r) {
        auto& rec = records[r];
        // v >= 5: 8 bytes of per-record metadata before the children
        // (the disasm reads them as 2×u32 or 1×f64 into +0x28 of the
        // record struct — we just store the bytes).
        if (magic >= 5) {
            std::uint8_t buf[8];
            ar.read_bytes(buf, 8);
            std::memcpy(&rec.longitudinal_position, buf, 8);
        }
        // 5-iteration inner loop.  Each inner pair: read child_a; if
        // child_a is "non-empty", read child_b + 6 per-slot trailer
        // blocks (the W-section variant of F_Section's FUN_005e69a0).
        for (int j = 0; j < 5; ++j) {
            auto& inner = rec.inner[j];
            inner.child_a = ar.read_object();
            bool a_non_empty = child_a_u32_at(inner.child_a, 0x0c) != 0
                            || child_a_u32_at(inner.child_a, 0x10) != 0
                            || child_a_u32_at(inner.child_a, 0x14) != 0
                            || child_a_u32_at(inner.child_a, 0x18) != 0
                            || child_a_u32_at(inner.child_a, 0x20) != 0;
            if (a_non_empty) {
                inner.child_b = ar.read_object();
                // 6 W-section trailer blocks (FUN_005e71f0) per inner pair.
                const int slot_offsets[6] = { 0x0c, 0x10, 0x18, 0x14, 0x1c, 0x20 };
                for (int slot = 0; slot < 6; ++slot) {
                    read_wsection_trailer_block(inner.interpolation_blocks[slot],
                        child_a_u32_at(inner.child_a, slot_offsets[slot]));
                }
            }
        }
    }
    body_complete = true;
}


// ---- Registration --------------------------------------------------------

namespace {
template <class T>
void reg(ClassRegistry& r) {
    r.register_class(
        T::kClassName,
        []() -> std::shared_ptr<PersistentObject> {
            return std::make_shared<T>();
        });
}
}  // namespace

void register_all_descriptors() {
    auto& r = ClassRegistry::instance();

    // FULL tier
    reg<TrackDescriptor>(r);
    reg<LodSwitchDescriptor>(r);
    reg<StateSwitchDescriptor>(r);
    reg<ProgressiveModificationDescriptor>(r);
    reg<TransformDescriptor>(r);
    reg<SegmentDescriptor>(r);
    reg<TextureDescriptor>(r);
    reg<PlainVertexListDescriptor>(r);
    reg<GeometryDescriptor>(r);
    reg<GroupDescriptor>(r);
    reg<TriListDescriptor>(r);
    reg<TriStripDescriptor>(r);
    reg<TriFanDescriptor>(r);
    reg<EmptyDescriptor>(r);
    reg<PortalDescriptor>(r);
    reg<BillboardDescriptor>(r);
    reg<AnimatedTransformDescriptor>(r);
    reg<ShapeDescriptor>(r);
    reg<AppearanceDescriptor>(r);
    reg<GroupingNodeDescriptor>(r);
    reg<PointLightDescriptor>(r);
    reg<AppNodeDescriptor>(r);
    reg<InfiniteLightDescriptor>(r);

    // FULL tier — promoted from PARTIAL (first pass)
    reg<NodeDescriptor>(r);
    reg<ChildNodeDescriptor>(r);
    reg<VertexListDescriptor>(r);
    reg<SelfLightingDescriptor>(r);
    reg<BiCubicPatchDescriptor>(r);
    reg<ProgressiveMeshDescriptor>(r);
    reg<TrackDetailDescriptor>(r);
    reg<TSODescriptor>(r);
    reg<TSOReferenceDescriptor>(r);
    reg<TextureCoordsDescriptor>(r);
    reg<TrackGrooveDescriptor>(r);
    reg<TrackRaceLineDescriptor>(r);

    // FULL tier — promoted from PARTIAL (second pass: vertex-list family + sections)
    reg<MorphVertexListDescriptor>(r);
    reg<LodMorphVertexListDescriptor>(r);
    reg<StateMorphVertexListDescriptor>(r);
    reg<RegionMorphVertexListDescriptor>(r);
    reg<SpanningVertexListDescriptor>(r);
    reg<X_SectionDescriptor>(r);
    reg<F_SectionDescriptor>(r);
    reg<W_SectionDescriptor>(r);
}

}  // namespace opennr::papyrus
