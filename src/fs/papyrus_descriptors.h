#pragma once

// Per-class read() implementations for the Papyrus typed-stream object
// hierarchy, on top of the new `papyrus::Archive` framework.
//
// Each class here inherits from `papyrus::PersistentObject`, exposes
// `kClassName` (matching the on-disk class-name token), and overrides
// `read(Archive&)` to consume its body bytes.  They register with the
// global `papyrus::ClassRegistry` via `register_all_descriptors()` so
// `Archive::read_object()` can instantiate them by name.
//
// This is the V2 of `src/fs/descriptors.h` — the legacy header keeps
// the POD structs that the token-scan parser (`object_tree.cpp`) and
// `object_tool` still consume via `std::variant`.  The two live in
// parallel until the legacy callers migrate.
//
// Body completeness tiers:
//
//   FULL  — read() consumes the entire body and the Archive cursor is
//           ready for the next object.  Suitable for chaining
//           Archive::read_object() calls back-to-back.
//
//   PARTIAL — read() consumes only the universal header plus the
//           prefix we trust.  The remaining body bytes are NOT
//           consumed; chaining reads after a PARTIAL descriptor will
//           desynchronize.  Marked `body_complete = false`.
//
// Tier assignments are documented per class.  Promoting a class from
// PARTIAL to FULL is a focused follow-up that requires nailing the
// body length (either by decompiling its read() in rts.dll-equivalent
// runtime code, or by deduction from sample files).

#include "fs/papyrus_archive.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opennr::papyrus {

// ---- Universal descriptor header -----------------------------------------
//
// Every descriptor body begins with (u32 version, u32 name_length,
// char name[name_length]) where name_length includes the trailing NUL
// and is 0 when the node is anonymous.
struct DescriptorHeader {
    std::uint32_t version     = 0;
    std::uint32_t name_length = 0;   // includes the trailing NUL
    std::string   name;
};

DescriptorHeader read_descriptor_header(Archive& ar);

// ---- Base for every typed-stream descriptor in this module --------------
//
// Provides the universal-header read + `body_complete` flag.  Subclasses
// implement `read_body(Archive&)` for their class-specific fields.
class DescriptorBase : public PersistentObject {
public:
    DescriptorHeader header;
    bool body_complete = false;   // true when read() consumed the whole body

    void read(Archive& ar) override final {
        header = read_descriptor_header(ar);
        read_body(ar);
    }

protected:
    virtual void read_body(Archive& ar) = 0;
};

// =========================================================================
// TIER FULL — read() consumes the entire body
// =========================================================================

// TrackDescriptor — root of every `.ptf` file.  Body recovered from
// NR2003.exe FUN_005e52c0 (TrackDescriptor::read, vtable[0] of
// 0x006f89c0):
//
//   universal_header                              (DescriptorBase: magic_node + lp_string)
//   u32 version                                    cap <= 8, default 8
//   u32 num_segments                               @ this+0x10
//
//   [version >= 3 AND stream_version == 0]:
//     u8  flag                                    @ this+0x30
//     -- For SHIPPED tracks (version 8) the SMALL branch is taken:
//        f64 compatibility_scalar_a               @ this+0x31 (default 1.0)
//        f64 compatibility_scalar_b               @ this+0x39 (default 1.0)
//        f64 compatibility_scalar_c               @ this+0x41 (default 1.0)
//        u32 compatibility_word                   @ this+0x2c
//     -- For version 3..7 (legacy) the BIG compatibility branch consumes
//        5 doubles (v3-v4), 13 doubles (v5), or 21 doubles (v6-v7).
//        They are obsolete authoring fields; the runtime retains its 0.2
//        defaults for the three modern scale slots.
//
//   [version >= 7 OR stream_version != 0]:
//     u32 num_records_e                            @ this+0x49
//     num_records_e × 48 bytes  (raw)              @ this+0x4d
//       +0x00..0x0f authoring RNG/state bytes (runtime generator ignores)
//       +0x10..0x1f f32 amplitude_mm[4]
//       +0x20..0x2f f32 distance_knots_m[4]
//
//   [version == 2 OR version > 3]:
//     transferPersistentObject &single_child       @ this+0x24
//     u32 num_children_c                           @ this+0x1c
//     num_children_c × transferPersistentObject    @ this+0x20  (array)
//
//   (always, unconditional)
//   u32 num_children_b                             @ this+0x14
//   num_children_b × transferPersistentObject      @ this+0x18  (array)
//   num_segments × transferPersistentObject        @ this+0x0c  (array of SegmentDescriptors)
//
// In atlanta.ptf:
//   - `single_child` is typically a renderable scene-graph node
//     (LodSwitch/Group/Shape subtree drawing the track surface + walls).
//   - `children_b` is the list of TSOReferenceDescriptor instances
//     (track-side `.3do` references — grandstands, billboards, etc.;
//     216 on atlanta).
//   - `children_c` is the list of TrackDetailDescriptor instances
//     (`tsd_*.mip`-textured trackside details; 13 on atlanta).
//     (Earlier revisions of this comment had the two lists swapped.)
//   - `segments` is the `num_segments`-long SegmentDescriptor array
//     (one per longitudinal track segment; physics-side queryable).
class TrackDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "TrackDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t version       = 0;
    std::int32_t  num_segments  = 0;

    // Version-conditional scalars (v8 SMALL branch).  The native constructor
    // seeds 0.2 for legacy versions before consuming their obsolete fields.
    std::uint8_t  flag          = 0;
    double        scalar_a      = 0.2;
    double        scalar_b      = 0.2;
    double        scalar_c      = 0.2;
    std::uint32_t scalar_d_u32  = 0;

    // Raw-valued legacy compatibility fields, in disk order.  NR2003 reads
    // these into temporaries and does not use them to replace scalar_a..c.
    std::vector<double> legacy_scalars;

    // version-conditional raw record block (v >= 7).
    std::uint32_t num_records_e = 0;
    std::vector<std::uint8_t> records_e_raw;   // decoded at physics boundary

    // v > 3 only:
    std::shared_ptr<PersistentObject>             single_child;
    std::vector<std::shared_ptr<PersistentObject>> children_c;   // TSO refs etc.

    // Always:
    std::vector<std::shared_ptr<PersistentObject>> children_b;   // TrackDetails etc.
    std::vector<std::shared_ptr<PersistentObject>> segments;     // SegmentDescriptor[num_segments]

    // Number of segment objects parsed. A failed segment now rejects the
    // archive, so successful TrackDescriptors always equal num_segments.
    std::int32_t segments_parsed_count = 0;

protected:
    void read_body(Archive& ar) override;
};

// LodSwitchDescriptor — selects one child by distance to camera.  Body
// from NR2003.exe 0x005e04f0:
//   u32 flag_a=1, flag_b=1, 48 bbox bytes  (Transformable parent)
//   u32 magic                              (per-class version, <=2)
//   u32 num_lod_levels                     @ this+0x3c
//   3 × f64 centre_x, centre_y, centre_z   @ this+0x48, +0x50, +0x58
//   [if magic > 1]: u8 flag                @ this+0x60
//   transferPointer(distances, num_lod_levels * 8):
//     u32 alloc_size = num_lod_levels * 8
//     num_lod_levels × f64 switch-distances
//   transferPointerButNotData(children, num_lod_levels * 4):
//     u32 alloc_size = num_lod_levels * 4
//   num_lod_levels × transferPersistentObject
class LodSwitchDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "LodSwitchDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t flag_a = 0, flag_b = 0;
    std::uint8_t  bbox[48] = {};
    std::uint32_t magic = 0;
    std::uint32_t num_lod_levels = 0;
    double centre_x = 0, centre_y = 0, centre_z = 0;
    std::uint8_t flag = 0;        // present only when magic > 1
    std::vector<double> switch_distances;
    std::vector<std::shared_ptr<PersistentObject>> children;

protected:
    void read_body(Archive& ar) override;
};

// StateSwitchDescriptor — selects one child by a named state variable
// (e.g. "Weather", "Weekend", "Day_Night").  Body from NR2003.exe
// 0x005e0640:
//   u32 flag_a=1, flag_b=1, 48 bbox bytes  (Transformable parent)
//   u32 magic                              (per-class version, >=1)
//   transferString(state_name)             @ this+0x3d
//   [if magic >= 2]: u8 flag               @ this+0x3c
//   u32 num_states                         @ this+0x41
//   f64 default_value                      @ this+0x45
//   transferPointer(state_values, num_states*8 + 8):
//     u32 alloc_size = num_states * 8 + 8  (= (num_states+1) doubles)
//     (num_states+1) × f64
//   transferPointerButNotData(children, num_states * 4):
//     u32 alloc_size = num_states * 4
//   num_states × transferPersistentObject
//
// At read time, if state_name is one of "Weather"/"Weekend"/"Day_Night"
// the flag is forced to 1 (overrides the on-disk value).
class StateSwitchDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "StateSwitchDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t flag_a = 0, flag_b = 0;
    std::uint8_t  bbox[48] = {};
    std::uint32_t magic = 0;
    std::string   state_name;
    std::uint8_t  flag = 0;
    std::uint32_t num_states = 0;
    double        default_value = 0;
    std::vector<double> state_values;       // size = num_states + 1
    std::vector<std::shared_ptr<PersistentObject>> children;

protected:
    void read_body(Archive& ar) override;
};

class ProgressiveModificationDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "ProgressiveModificationDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::int32_t change_num_vertices   = 0;
    std::int32_t num_modified_vertices = 0;
    std::int32_t change_num_tris       = 0;
    std::int32_t num_modified_tris     = 0;

protected:
    void read_body(Archive& ar) override;
};

class TransformDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "TransformDescriptor";
    std::string_view class_name() const override { return kClassName; }

    // Body shape per NR2003 FUN_005e07f0 (TransformDescriptor::read):
    //   ChildNodeDescriptor parent (FUN_005e03c0):
    //     NodeDescriptor parent (FUN_005e0360):
    //       universal header (consumed by DescriptorBase::read)
    //       u32 magic_node = 1
    //     u32 magic_child_node = 1
    //     48 bbox bytes
    //   u32 marker = 1                              (TransformDescriptor magic)
    //   6 unaligned doubles: tx, ty, tz, yaw, pitch, roll
    //   child object (transferPersistentObject @ +0x6c)
    std::uint32_t flag_a = 0, flag_b = 0, marker = 0;
    std::uint8_t  bbox[48] = {};
    double tx = 0, ty = 0, tz = 0;
    double yaw = 0, pitch = 0, roll = 0;
    std::shared_ptr<PersistentObject> child;

protected:
    void read_body(Archive& ar) override;
};

// SegmentDescriptor — one segment of the track centreline.  Body from
// NR2003.exe FUN_005e5b40 (vtable[0] of 0x006f89d8):
//
//   universal_header (NodeDescriptor parent — u32 magic_node = 1)
//   u32 version (default 2; cap < 3 when stream_version == 0)
//
//   u32 num_x_sections                          @ this+0x42
//   num_x_sections × transferPersistentObject   @ this+0x4e (array)
//   u32 num_f_sections                          @ this+0x46
//   num_f_sections × transferPersistentObject   @ this+0x52 (array)
//   u32 num_w_sections                          @ this+0x4a
//   num_w_sections × transferPersistentObject   @ this+0x56 (array)
//
//   u32 segment_kind                            @ this+0x0c  (-1 = sentinel)
//   [if segment_kind != -1]:
//     f64 pos_a                                 @ this+0x10
//     f64 pos_b                                 @ this+0x18
//     f64 angle_c                               @ this+0x20  (validated bounds)
//     f64 pos_d                                 @ this+0x28
//     f64 pos_e                                 @ this+0x30
//     f64 angle_f                               @ this+0x38  (validated bounds)
//     u8  flag_a                                @ this+0x40
//     u8  flag_b                                @ this+0x41
//     u32 num_records_12                        @ this+0x5a
//     num_records_12 × {f32 start,f32 end,i32 selector} @ this+0x62
//     u32 num_records_16                        @ this+0x5e
//     num_records_16 × {f32 a0,f32 a1,f32 b0,f32 b1}   @ this+0x66
//
// 0x005105c0 compiles both arrays to affine start/delta records and
// 0x00513eaa consumes them in the surface-detail height path. The clean-room
// compiler lives in physics/track_surface_detail.cpp. They remain raw here for
// format preservation and are never grip/width values.
//
// Critical: the section refs are TYPED-OBJECT references
// (transferPersistentObject), NOT bare u32 handles.  An earlier
// clean-room read used `read_u32` for each which silently desync'd the
// stream whenever a fresh X_/F_/W_Section was encountered inline.
class SegmentDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "SegmentDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t version = 0;
    std::vector<std::shared_ptr<PersistentObject>> x_sections;
    std::vector<std::shared_ptr<PersistentObject>> f_sections;
    std::vector<std::shared_ptr<PersistentObject>> w_sections;
    std::int32_t               segment_kind = -1;
    // Only valid when segment_kind != -1:
    double pos_a = 0, pos_b = 0, angle_c = 0;
    double pos_d = 0, pos_e = 0, angle_f = 0;
    std::uint8_t flag_a = 0, flag_b = 0;
    std::vector<std::uint8_t> records_12_raw;    // num_records_12 * 12 bytes
    std::vector<std::uint8_t> records_16_raw;    // num_records_16 * 16 bytes

protected:
    void read_body(Archive& ar) override;
};

// TextureDescriptor — single texture reference.  Body from NR2003.exe
// 0x005e29e0:
//   u32 magic = 2                       (per-class version, <=2)
//   transferString(texture_name)        e.g. "series_flagger.mip\0"
//   u8 flag                             (default 0 on version<2)
//
// The previous decoder believed this descriptor also carried a 25-byte
// raw blob + 12 doubles for a UV matrix.  In fact those 124 bytes
// belong to the *parent* AppearanceDescriptor's color/material trailer.
// Texture's read() in NR2003 also calls vtable+0x18 which loads the
// .mip file from disk — that's not Archive data, it's file I/O.
class TextureDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "TextureDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t magic = 0;
    std::string   texture_name;
    std::uint8_t  flag = 0;

protected:
    void read_body(Archive& ar) override;
};

// PlainVertexListDescriptor — static vertex array.  Body from NR2003.exe
// 0x005e11f0 + 0x005e0c40 (UV / attribute reader):
//
//   universal_header                (version + name)
//   u32 vertex_list_magic = 1
//   u32 plain_magic       = 1
//   u32 num_vertices                (cap 0x4098)
//   7 × transferPointer(N*8):
//       channel 0..2 = positions x/y/z   (struct fields +0x74..+0x7c)
//       channel 3    = reserved/legacy alpha (struct field +0x80, always empty
//                                                   on shipped files)
//       channel 4..6 = normals x/y/z     (struct fields +0x84..+0x8c)
//   u32 attr_magic = 1..3 (per-class version of the UV-attribute table)
//   24 × transferPointer(N*8):
//       channel 0..23 = u, v, then 22 reserved attribute slots
//                       (most empty on disk).
class PlainVertexListDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "PlainVertexListDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t       type_code = 0;     // VertexList magic == 1
    std::uint32_t       flag      = 0;     // PVL own magic == 1
    std::int32_t        num_vertices = 0;
    std::vector<double> positions_x, positions_y, positions_z;
    std::vector<double> normals_x,   normals_y,   normals_z;

    // The unused channel between positions and normals (struct +0x80 in
    // NR2003).  Empty on every shipped file we've seen but consumed by
    // FUN_005e11f0 so the stream cursor stays in sync.
    std::vector<double> reserved_channel;

    // attr_magic = the u32 read first by FUN_005e0c40.  Constrained to
    // <= 3 in stream_version==0 files.  Preserved for round-trip writes.
    std::uint32_t       attr_magic = 0;

    // The UV / attribute table is 24 fixed slots.  Slots 0 and 1 are U
    // and V respectively on shipped files; the remaining 22 are
    // reserved attribute channels that are empty on disk most of the
    // time but read unconditionally by FUN_005e0c40.
    static constexpr int kAttributeChannelCount = 24;
    std::vector<std::vector<double>> uv_channels;

protected:
    void read_body(Archive& ar) override;
};

// GeometryDescriptor — wraps a vertex list and a primitive descriptor.
// Body from NR2003.exe 0x005e2960:
//   u32 magic = 1
//   transferPersistentObject(&vertex_list)
//   transferPersistentObject(&primitive)
//
// The previous decoder read (u32 type, u32 unk_a, u32 unk_b) here —
// wrong.  Those fields are not in the typed stream.
class GeometryDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "GeometryDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t magic = 0;
    std::shared_ptr<PersistentObject> vertex_list;
    std::shared_ptr<PersistentObject> primitive;

protected:
    void read_body(Archive& ar) override;
};

// GroupDescriptor — a scene-graph node with N children.  Body layout
// recovered from `GroupDescriptor::read()` @ NR2003.exe 0x005e0430 and
// its chain through 0x005e03c0, 0x005e0360, 0x005e0300:
//
//   u32 flag_a = 1                              (NodeDescriptor::read)
//   u32 flag_b = 1                              (TransformableNode::read)
//   48 bytes (presumed 6 doubles -- bbox or transform)
//   u32 magic = 1                               (Group::read own)
//   u32 num_children
//   u32 alloc_size = num_children * 4           (from transferPointerButNotData)
//   N × transferPersistentObject(&children[i])  (recursive deserialization)
//
// Note: this descriptor is FULL only when every child class is also FULL
// (else a child's read_body() leaves bytes in the stream and the next
// child's read() begins mid-body, desynchronizing the read).
class GroupDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "GroupDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t flag_a = 0, flag_b = 0;
    std::uint8_t  bbox[48] = {};
    std::uint32_t magic = 0;
    std::uint32_t num_children = 0;
    std::vector<std::shared_ptr<PersistentObject>> children;

protected:
    void read_body(Archive& ar) override;
};

// TriListDescriptor — triangle list primitive.  Body layout recovered
// from `TriListDescriptor::read()` @ NR2003.exe 0x005e27a0 and its
// chain through 0x005e25f0, 0x005e0300:
//
//   u32 flag_a = 1                              (PrimitiveDescriptor::read)
//   transferPersistentObject(&appearance)       (inline child -- typically
//                                                AppearanceDescriptor, but
//                                                any registered class works)
//   u32 magic = 1                               (TriList::read own)
//   u32 num_indices (cap 0x4098)
//   u32 alloc_size = num_indices * 4            (from transferPointer)
//   N × u32 indices
//
// FULL only when the inline-child class is also FULL.
class TriListDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "TriListDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t flag_a = 0;
    // The persistent object reference at struct offset +0xc of every
    // PrimitiveDescriptor.  On disk this is a polymorphic "chain next"
    // pointer: Geometry's primitive field is the HEAD of a linked list,
    // and each primitive's `next_primitive` is the next link.  End of
    // chain is signaled by a null obj_handle.  The legacy decoder
    // mis-called this "appearance" because the field type was unknown.
    std::shared_ptr<PersistentObject> next_primitive;
    std::uint32_t magic = 0;
    std::uint32_t num_indices = 0;
    std::vector<std::uint32_t> indices;

protected:
    void read_body(Archive& ar) override;
};

// TriStripDescriptor / TriFanDescriptor share byte-identical read()
// bodies with TriListDescriptor (verified against NR2003.exe 0x005e2660
// / 0x005e2700 / 0x005e27a0 — same 153 B vtable[0] implementations).
// The semantic difference is at draw time, not on disk.
class TriStripDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "TriStripDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t flag_a = 0;
    // Polymorphic chain-next pointer; see TriListDescriptor for layout.
    std::shared_ptr<PersistentObject> next_primitive;
    std::uint32_t magic = 0;
    std::uint32_t num_indices = 0;
    std::vector<std::uint32_t> indices;

protected:
    void read_body(Archive& ar) override;
};

class TriFanDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "TriFanDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t flag_a = 0;
    // Polymorphic chain-next pointer; see TriListDescriptor for layout.
    std::shared_ptr<PersistentObject> next_primitive;
    std::uint32_t magic = 0;
    std::uint32_t num_indices = 0;
    std::vector<std::uint32_t> indices;

protected:
    void read_body(Archive& ar) override;
};

// ShapeDescriptor — renderable shape (appearance + geometry).  Body
// from NR2003.exe 0x005e2fb0:
//   u32 magic_node = 1                  (NodeDescriptor parent)
//   u32 magic = 1
//   transferPersistentObject(&appearance)
//   transferPersistentObject(&geometry)
class ShapeDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "ShapeDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic_node = 0;
    std::uint32_t magic = 0;
    std::shared_ptr<PersistentObject> appearance;
    std::shared_ptr<PersistentObject> geometry;
protected:
    void read_body(Archive& ar) override;
};

// AppearanceDescriptor — material binding (7 texture slots + color
// scalars).  Body from NR2003.exe 0x005e2df0:
//   u32 magic = 2                                (per-class version, <=2)
//   7 × transferPersistentObject(&texture_slot[i])
//   3 × f64 ambient r,g,b
//   3 × f64 diffuse r,g,b
//   3 × f64 specular r,g,b
//   f64 shinyness
//   f64 reflectivity
//   f64 opacity
//   f32 envmap_index
class AppearanceDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "AppearanceDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic = 0;
    std::shared_ptr<PersistentObject> texture_slots[7];
    double ambient_r = 0, ambient_g = 0, ambient_b = 0;
    double diffuse_r = 0, diffuse_g = 0, diffuse_b = 0;
    double specular_r = 0, specular_g = 0, specular_b = 0;
    double shinyness = 0, reflectivity = 0, opacity = 0;
    float  envmap_index = 0;
protected:
    void read_body(Archive& ar) override;
};

// GroupingNodeDescriptor — byte-identical to LodSwitchDescriptor at
// NR2003 0x005e0500 (the decompile shows the same function as
// LodSwitch's FUN_005e04f0).  Format is the LOD-switch wire format.
class GroupingNodeDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "GroupingNodeDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t flag_a = 0, flag_b = 0;
    std::uint8_t  bbox[48] = {};
    std::uint32_t magic = 0;
    std::uint32_t num_children = 0;
    double centre_x = 0, centre_y = 0, centre_z = 0;
    std::uint8_t flag = 0;
    std::vector<double> switch_distances;
    std::vector<std::shared_ptr<PersistentObject>> children;
protected:
    void read_body(Archive& ar) override;
};

// PointLightDescriptor — byte-identical to ShapeDescriptor at NR2003
// 0x005e3020 (decompile shows same FUN_005e2fb0).  Format is the
// Shape wire format (Node parent + magic + 2 children).
// PointLightDescriptor — point-light source.  Body from NR2003 FUN_005e3030:
//   NodeDescriptor parent: u32 magic_node = 1
//   u32 magic (v3 in shipped files; v<2 supplies defaults; v<3 uses the
//     legacy color-vector × diffuse/ambient-scale representation)
//   double radius_squared @ this+0x24  (magic >= 2; else default 4.0)
//   3 floats diffuse RGB  @ this+0xc   (only when magic == 3)
//   3 floats ambient RGB  @ this+0x18  (only when magic == 3)
//   double position_x @ this+0x2c
//   double position_y @ this+0x34
//   double position_z @ this+0x3c
class PointLightDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "PointLightDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic_node = 0;
    std::uint32_t magic = 0;
    double radius_squared = 4.0;
    float diffuse_r = 1.0f, diffuse_g = 1.0f, diffuse_b = 1.0f;
    float ambient_r = 1.0f, ambient_g = 1.0f, ambient_b = 1.0f;
    double legacy_color[4] = {};
    double legacy_diffuse_scale = 0.0;
    double legacy_ambient_scale = 0.0;
    double position_x = 0.0, position_y = 0.0, position_z = 0.0;
protected:
    void read_body(Archive& ar) override;
};

// AppNodeDescriptor — application-defined hook node.  Body from
// NR2003.exe 0x005e3600:
//   Transformable parent (flag_a + flag_b + 48 bbox)
//   u32 magic = 1
//   u32 app_id          @ this+0x3c
//   u32 data_size       @ this+0x44
//   transferBytes(buf, data_size)   raw bytes when data_size > 0
//   transferPersistentObject(&child)
class AppNodeDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "AppNodeDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t flag_a = 0, flag_b = 0;
    std::uint8_t  bbox[48] = {};
    std::uint32_t magic = 0;
    std::uint32_t app_id = 0;
    std::uint32_t data_size = 0;
    std::vector<std::uint8_t> data;
    std::shared_ptr<PersistentObject> child;
protected:
    void read_body(Archive& ar) override;
};

// InfiniteLightDescriptor — directional light.  Body from NR2003.exe
// 0x005e31b0 (Node parent + own fields, supports v1 and v2):
//   Node parent (u32 magic_node = 1)
//   u32 magic                                   (1 or 2)
//   [if magic < 2]:  // legacy path — reads doubles, converts to floats
//     4 × f64 color vector (component 3 is retained but unused at runtime)
//     f64 diffuse_scale, f64 ambient_scale
//     diffuse_rgb = color[0..2] * diffuse_scale
//     ambient_rgb = color[0..2] * ambient_scale
//   [if magic == 2]:
//     3 × f32 diffuse_rgb                       @ this+0x0c
//     3 × f32 ambient_rgb                       @ this+0x18
//   3 × f64 axis_ijk                            @ this+0x24/+0x2c/+0x34
//
// The reader supports every native version 0..2 and preserves the legacy
// source values as well as the converted colors.
class InfiniteLightDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "InfiniteLightDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic_node = 0;
    std::uint32_t magic = 0;
    float diffuse_r = 0, diffuse_g = 0, diffuse_b = 0;
    float ambient_r = 0, ambient_g = 0, ambient_b = 0;
    double legacy_color[4] = {};
    double legacy_diffuse_scale = 0.0;
    double legacy_ambient_scale = 0.0;
    double axis_i = 0, axis_j = 0, axis_k = 0;
protected:
    void read_body(Archive& ar) override;
};

// EmptyDescriptor — leaf node with no children, body is just the
// universal header.  FUN_005e0300 in NR2003 is BOTH the universal-
// header reader AND Empty's vtable[0], i.e. Empty extends Object
// directly with no fields.
class EmptyDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "EmptyDescriptor";
    std::string_view class_name() const override { return kClassName; }
protected:
    void read_body(Archive&) override { body_complete = true; }
};


// PortalDescriptor — cell/portal occlusion plane.  Body from NR2003.exe
// 0x005e3300:
//   u32 magic_node = 1                 (NodeDescriptor parent)
//   u32 magic = 1
//   transferPersistentObject(&target)  (the cell on the other side)
//   u32 num_indices
//   transferPointer(&indices, num_indices * 4)
//     -> u32 alloc_size (= num_indices * 4)
//     -> num_indices × u32
class PortalDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "PortalDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic_node = 0;
    std::uint32_t magic = 0;
    std::shared_ptr<PersistentObject> target;
    std::uint32_t num_indices = 0;
    std::vector<std::uint32_t> indices;
protected:
    void read_body(Archive& ar) override;
};

// BillboardDescriptor — always-face-camera node.  Body from NR2003.exe
// 0x005e0980 (Transformable parent + own fields):
//   u32 flag_a = 1, flag_b = 1, 48 bbox bytes   (Transformable parent)
//   u32 magic = 1
//   3 × f64 pivot/translation XYZ @ +0x3c
//   3 × f64 facing-axis XYZ       @ +0x54
//   transferPersistentObject(&child)
class BillboardDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "BillboardDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t flag_a = 0, flag_b = 0;
    std::uint8_t bbox[48] = {};
    std::uint32_t magic = 0;
    double pivot_x = 0, pivot_y = 0, pivot_z = 0;
    double axis_x = 0, axis_y = 0, axis_z = 0;
    std::shared_ptr<PersistentObject> child;
protected:
    void read_body(Archive& ar) override;
};

// AnimatedTransformDescriptor — keyframed transform.  Extends Transform.
// Body from NR2003 FUN_005e08e0 (AnimatedTransformDescriptor::read):
//   TransformDescriptor parent (FUN_005e07f0 — see TransformDescriptor):
//     u32 magic_node = 1, u32 magic_child_node = 1, 48 bbox bytes
//     u32 transform_marker = 1
//     6 doubles at +0x3c..+0x64
//     child object (transferPersistentObject @ +0x6c)  ← e.g. a GroupDescriptor
//   u32 magic = 1
//   transferString(&channel_name)                              [@ +0x70]
//   u32 num_keyframes                                          [@ +0x74]
//   transferPointer(&keyframes_raw, num_keyframes * 32)        [@ +0x78]
//     -> u32 alloc_size (= num_keyframes * 32)
//     -> num_keyframes × 32 bytes: u32 timestamp, float quat[4],
//        float translation[3].  Confirmed by rend_dxg.dll FUN_10025330.
//
// IMPORTANT: the second triple of doubles (+0x54..+0x64) is NOT the
// same semantically as TransformDescriptor's yaw/pitch/roll, even
// though the byte layout matches.  Per
// docs/formats/3do_descriptor_layouts.md (Ghidra dump @ 005D9C10), the
// fields are an **axis-angle rotation axis** (axis_x, axis_y, axis_z);
// the rotation angle is sampled from the keyframe stream at runtime.
// For a static rest-pose render with no keyframe sampling, the angle
// is undefined / 0, so these axis components must NOT be applied as
// Euler-style yaw/pitch/roll — doing so deforms every animated joint
// in shipped pit-crew / animated tire models.
struct AnimationKeyframe {
    std::uint32_t timestamp = 0;
    float rotation[4] = {0.f, 0.f, 0.f, 1.f};
    float translation[3] = {0.f, 0.f, 0.f};
};

class AnimatedTransformDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "AnimatedTransformDescriptor";
    std::string_view class_name() const override { return kClassName; }
    // Transform inherited:
    std::uint32_t flag_a = 0, flag_b = 0;
    std::uint8_t  bbox[48] = {};
    std::uint32_t transform_marker = 0;
    double tx = 0, ty = 0, tz = 0;
    // Axis-angle rotation axis (see comment above).
    double axis_x = 0, axis_y = 0, axis_z = 0;
    std::shared_ptr<PersistentObject> child;
    // Own:
    std::uint32_t magic = 0;
    std::string   channel_name;
    std::uint32_t num_keyframes = 0;
    std::vector<std::uint8_t> keyframes_raw;   // num_keyframes × 32 bytes
    std::vector<AnimationKeyframe> keyframes;
protected:
    void read_body(Archive& ar) override;
};

// =========================================================================
// More FULL-tier classes promoted from PARTIAL.
// =========================================================================

// Abstract base helpers — these classes are registered with the typed-
// stream framework but never appear as standalone instances in stock
// .3do/.ptf files (they're parent classes only).  We still implement
// read_body() so the registry can dispatch if a stream ever names them.
//
// NodeDescriptor::read at NR2003 0x005e0360:
//   universal_header (consumed by DescriptorBase::read)
//   u32 magic = 1
//
// VertexListDescriptor::read at NR2003 0x005e0be0 has byte-identical
// shape to NodeDescriptor::read.  ChildNodeDescriptor has no known
// concrete instances; we mirror NodeDescriptor's structure (the only
// plausible shape for an abstract registered base — a single magic
// guard word).  If a stream ever names it, the magic check will catch
// a mismatch.
class NodeDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "NodeDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic = 0;
protected:
    void read_body(Archive& ar) override;
};

class ChildNodeDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "ChildNodeDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic = 0;
protected:
    void read_body(Archive& ar) override;
};

class VertexListDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "VertexListDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic = 0;
protected:
    void read_body(Archive& ar) override;
};

// SelfLightingDescriptor — TransformableNode-derived node carrying explicit
// diffuse, specular, and ambient override channels.  Body from NR2003
// 0x005e0a70:
//   TransformableNode parent: u32 flag_a, u32 flag_b, 48 bbox bytes
//   u32 magic (== 1)
//   u8 diffuse_set, specular_set, ambient_set @ +0x3c..+0x3e
//   3 × f64 diffuse RGB  @ +0x3f
//   3 × f64 specular RGB @ +0x57
//   3 × f64 ambient RGB  @ +0x6f
//   transferPersistentObject child @ +0x87
class SelfLightingDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "SelfLightingDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t flag_a = 0, flag_b = 0;
    std::uint8_t  bbox[48] = {};
    std::uint32_t magic = 0;
    std::uint8_t diffuse_set = 0, specular_set = 0, ambient_set = 0;
    double diffuse_r = 0, diffuse_g = 0, diffuse_b = 0;
    double specular_r = 0, specular_g = 0, specular_b = 0;
    double ambient_r = 0, ambient_g = 0, ambient_b = 0;
    std::shared_ptr<PersistentObject> child;
protected:
    void read_body(Archive& ar) override;
};

// BiCubicPatchDescriptor — Primitive-derived (header + magic + child)
// followed by a bicubic patch (4×4 control points + parameters).
// Body from NR2003 0x005e28e0 (calls Primitive::read FUN_005e25f0):
//   PrimitiveDescriptor parent:
//     u32 magic_prim (== 1)
//     transferPersistentObject &child @ +0xc
//   u32 magic (== 1)
//   16 × f32 @ +0x10 — 4×4 scalar patch grid
//   2 × f64  @ +0x50 — patch parameters
//   2 × (4 × f32) @ +0x60 — patch vectors
//
// NR2003 has no renderer registration or tessellation consumer for this
// descriptor, and its clone method copies +0x10..+0x7f verbatim. These names
// therefore describe the wire-level numeric structure, not an invented
// evaluation algorithm. No stock asset declares the class.
class BiCubicPatchDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "BiCubicPatchDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic_prim = 0;
    std::shared_ptr<PersistentObject> child;
    std::uint32_t magic = 0;
    std::array<float, 16> scalar_grid{};
    std::array<double, 2> parameters{};
    std::array<std::array<float, 4>, 2> vectors{};
protected:
    void read_body(Archive& ar) override;
};

// ProgressiveMeshDescriptor — Node-derived; reads three sub-objects,
// two scalar counts, and a variable-length list of progressive
// modification objects.  Body from NR2003 0x005e34b0:
//   NodeDescriptor parent (u32 magic = 1)
//   u32 magic (== 1)
//   transferPersistentObject child_a @ +0xc
//   transferPersistentObject child_b @ +0x10
//   transferPersistentObject child_c @ +0x14
//   u32 base_num_vertices @ +0x18
//   u32 base_num_tris     @ +0x1c
//   u32 num_modifications @ +0x20
//   transferPointerButNotData(&modifications, num_modifications * 4):
//     u32 alloc_size (= num_modifications * 4)
//   num_modifications × transferPersistentObject(&modifications[i])
class ProgressiveMeshDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "ProgressiveMeshDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic_node = 0;
    std::uint32_t magic = 0;
    std::shared_ptr<PersistentObject> child_a;
    std::shared_ptr<PersistentObject> child_b;
    std::shared_ptr<PersistentObject> child_c;
    std::uint32_t base_num_vertices = 0;
    std::uint32_t base_num_tris     = 0;
    std::uint32_t num_modifications = 0;
    std::vector<std::shared_ptr<PersistentObject>> modifications;
protected:
    void read_body(Archive& ar) override;
};

// TrackDetailDescriptor — header + version + child + per-record
// doubles + two pointer arrays.  Body from NR2003 0x005e8c30:
//   universal_header (DescriptorBase)
//   u32 magic (== 0 observed)
//   transferPersistentObject child_obj   (stored as scratch — see note)
//   5 × f64  @ +0x10, +0x18, +0x28, +0x20, +0x30
//   transferPointer(&buf32a, 32)        u32 size + 32 bytes
//   transferPointer(&buf32b, 32)        u32 size + 32 bytes
//
// Note: the binary reads `transferPersistentObject(&stack0xfffffff4)` into
// a stack temporary, then writes a fixed constant `uVar3 = 4` into
// this+0xc.  The child reference is consumed but immediately discarded;
// we still surface the parsed object so callers see what was on disk.
class TrackDetailDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "TrackDetailDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic = 0;
    std::shared_ptr<PersistentObject> child;
    double f_a = 0, f_b = 0, f_c = 0, f_d = 0, f_e = 0;
    std::uint8_t buf32a[32] = {};
    std::uint8_t buf32b[32] = {};
protected:
    void read_body(Archive& ar) override;
};

// TSODescriptor — references an external .tso file by name.  Body from
// NR2003 0x005e7bd0:
//   universal_header
//   u32 magic (<= 3)
//   transferString(name_a)              @ +0xc
//   [if magic >= 2]: transferString(name_b)  @ +0x10  (else "")
//   [if magic >= 3]: u8 flag                 @ +0x14  (else 0)
//   (runtime: when reading from disk, calls papyrus_loadPiffFile to
//    materialize the referenced .tso — pure deserialization skips this.)
class TSODescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "TSODescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic = 0;
    std::string   name_a;
    std::string   name_b;
    std::uint8_t  flag = 0;
protected:
    void read_body(Archive& ar) override;
};

// TSOReferenceDescriptor — references a TSO by ID plus a placement /
// orientation matrix.  Body from NR2003 0x005e8050:
//   universal_header
//   u32 magic (<= 6)
//   6 × f64  @ +0xc, +0x14, +0x1c, +0x24, +0x2c, +0x34  (transform components)
//   transferPersistentObject child  @ +0x50
//   if magic >= 4: u32 conditional_flag @ +0x40 (else 0)
//   u8 has_extra (always read)
//   if magic <  5: 32-byte block + 4 + 4 + 4 (inline payload at stack
//                   then memcpy'd into this+0x3c-pointed buffer)
//   else if (this+0x3c != 0): same 32+4+4+4 read into the pointed buffer
//   if "has_extra" tag bit:
//     u32 extra_count @ +0x48
//     extra_count × 0x28-byte records @ this+0x44 (allocated)
//   if magic >= 6: transferString(name) @ +0x4c (else "")
//
// We surface the doubles and child here; the conditional 32+12 byte
// inline block is captured as raw bytes when present, and the extra
// records as a raw byte buffer.
class TSOReferenceDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "TSOReferenceDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic = 0;
    double xform[6] = {};
    std::shared_ptr<PersistentObject> child;
    std::uint32_t conditional_flag = 0;
    bool has_extra = false;
    std::vector<std::uint8_t> inline_payload;   // 32 + 4 + 4 + 4 = 44 bytes when present
    std::vector<std::uint8_t> extra_records;    // extra_count × 0x28 bytes
    std::string name;
protected:
    void read_body(Archive& ar) override;
};

// TextureCoordsDescriptor — UV-coordinate set keyed by vertex count.
// Body from NR2003 0x005e0ed0:
//   universal_header
//   u32 magic (== 1)
//   u32 num_vertices @ +0x70  (cap 0x4098)
//   FUN_005e0c40(num_vertices):
//     u32 magic2 (== 3, gates whether the four "magic > 2" channels are read)
//     22 × transferPointer(channel[i], num_vertices * 8)
//
// Each channel is num_vertices doubles.  The channel index → semantic
// (U, V, ...) is not decoded here; we surface them as raw byte buffers
// keyed by their offset slot.
class TextureCoordsDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "TextureCoordsDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic = 0;
    std::uint32_t num_vertices = 0;
    std::uint32_t magic2 = 0;
    // 22 channels — indexed by their on-disk slot offset.  Channels 4..7
    // are only present when magic2 > 2 (we still create the vector entry
    // but leave it empty in that case).
    std::vector<std::vector<double>> channels;
protected:
    void read_body(Archive& ar) override;
};

// TrackGrooveDescriptor — narrow per-track tire-mark / dirt-line spline.
// Body from NR2003 0x005e8620:
//   universal_header
//   u32 magic (== 0)
//   u32 num_samples @ +0xc
//   transferPersistentObject &child @ +0x10
//   f64 length_or_radius @ +0x14
//   5 × transferPointer(channel[i], num_samples * 8)
//      (the 5 channels live at this+0x1c, +0x20, +0x24, +0x28, +0x2c)
class TrackGrooveDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "TrackGrooveDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic = 0;
    std::uint32_t num_samples = 0;
    std::shared_ptr<PersistentObject> child;
    double        scalar = 0;
    std::vector<double> channels[5];
protected:
    void read_body(Archive& ar) override;
};

// TrackRaceLineDescriptor — racing-line spline (driver pace lookups).
// Body from NR2003 0x005e8930:
//   universal_header
//   u32 magic (== 0)
//   u32 num_samples @ +0xc
//   transferPersistentObject &child @ +0x10
//   3 × f64 scalars @ +0x14, +0x24, +0x2c
//   2 × transferPointer(channel[i], num_samples * 8)
class TrackRaceLineDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "TrackRaceLineDescriptor";
    std::string_view class_name() const override { return kClassName; }
    std::uint32_t magic = 0;
    std::uint32_t num_samples = 0;
    std::shared_ptr<PersistentObject> child;
    double        scalars[3] = {};
    std::vector<double> channels[2];
protected:
    void read_body(Archive& ar) override;
};

// =========================================================================
// More FULL-tier classes — promoted in the second-pass May 2026 RE.
// =========================================================================

// MorphVertexListDescriptor — vertex list with per-frame morph data.
// Its 31 channels use the same schema as PlainVertexList: position XYZ plus
// auxiliary/alpha (4), normal XYZ (3), then the 24 vertex-attribute slots in
// six groups of four. Each
// channel is `num_frames` arrays, and each frame array is
// `max_vertices` doubles.
//
// Body from NR2003 0x005e1730 (decoded directly from disasm rather than
// the noisy decompile):
//   VertexList parent (u32 magic_parent == 1)
//   u32 magic            (native reader requires exactly 2)
//   u32 max_vertices     @ this+0xc, cap 0x4098
//   u32 num_frames       @ this+0x10
//   for each group g in [0..8) with sizes [4,3,4,4,4,4,4,4]:
//     for each channel c in [0..size[g]):
//       u32 alloc_size = num_frames * 4   (transferPointerButNotData)
//     for each frame i in [0..num_frames):
//       for each channel c in [0..size[g]):
//         u32 alloc_size = max_vertices * 8
//         max_vertices × f64
//
// We store the channels flat: `channels[31]` where each is a
// `vector<vector<double>>` (outer = frames, inner = max_vertices).
// The group structure is preserved by the named accessors below
// (group_offsets[g] tells you where group g starts in the flat array).
class MorphVertexListDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "MorphVertexListDescriptor";
    std::string_view class_name() const override { return kClassName; }

    // Number of channels per group in disk order: 4,3,4,4,4,4,4,4 = 31 total.
    static constexpr int kNumGroups = 8;
    static constexpr int kChannelsPerGroup[kNumGroups] = { 4, 3, 4, 4, 4, 4, 4, 4 };
    static constexpr int kTotalChannels = 31;

    std::uint32_t magic_parent = 0;
    std::uint32_t magic = 0;
    std::uint32_t max_vertices = 0;
    std::uint32_t num_frames = 0;

    // channels[c][frame][vertex] = double.  Outer size = 31 (kTotalChannels).
    std::vector<std::vector<std::vector<double>>> channels;

protected:
    void read_body(Archive& ar) override;
};

// LodMorphVertexListDescriptor — MorphVertexList + per-frame LOD-tag
// double.  Body from NR2003 0x005e2170:
//   MorphVertexList parent (full body)
//   u32 magic (== 1)
//   transferPointer(this+0x90, num_frames*8)  -- single channel of doubles
class LodMorphVertexListDescriptor : public MorphVertexListDescriptor {
public:
    static constexpr const char* kClassName = "LodMorphVertexListDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t lod_magic = 0;
    std::vector<double> lod_values;   // size = num_frames

protected:
    void read_body(Archive& ar) override;
};

// StateMorphVertexListDescriptor — byte-identical disk format to
// LodMorphVertexListDescriptor (NR2003 0x005e21e0 = 0x005e2170 modulo
// the vtable address).  The semantic difference is at draw time, not
// on disk.
class StateMorphVertexListDescriptor : public MorphVertexListDescriptor {
public:
    static constexpr const char* kClassName = "StateMorphVertexListDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t state_magic = 0;
    std::vector<double> state_values;   // size = num_frames

protected:
    void read_body(Archive& ar) override;
};

// RegionMorphVertexListDescriptor — VertexList with 8 named-region
// modification records.  Body from NR2003 0x005e1350:
//   VertexList parent (u32 magic_parent == 1)
//   u32 magic (== 1)
//   u32 num_vertices @ this+0x70  (cap 0x4098)
//   8 × transferPointer(channel[i], num_vertices*8)  @ this+0x74..+0x90
//   FUN_005e0c40 trailer (vertex-list trailer; see TextureCoordsDescriptor)
//   u32 num_regions   @ this+0x94
//   for each region r in [0..num_regions):
//     transferString(name)
//     u32 a, u32 b, u32 num_verts_in_region
//     8 × transferPointer(region_channel[i], num_verts_in_region*8)
//     1 × transferPointer(region_indices, num_verts_in_region*4)
struct RegionMorphRecord {
    std::string   name;
    std::uint32_t a = 0, b = 0;
    std::uint32_t num_verts_in_region = 0;
    std::vector<double>        channels[8];   // 8 channels of f64
    std::vector<std::uint32_t> indices;       // num_verts_in_region u32s
};

class RegionMorphVertexListDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "RegionMorphVertexListDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t magic_parent = 0;
    std::uint32_t magic = 0;
    std::uint32_t num_vertices = 0;
    std::vector<double> channels[8];          // 8 channels of f64

    // Trailer — see TextureCoordsDescriptor for the channel-set rules.
    std::uint32_t trailer_magic = 0;
    std::vector<std::vector<double>> trailer_channels;

    std::uint32_t num_regions = 0;
    std::vector<RegionMorphRecord> regions;

protected:
    void read_body(Archive& ar) override;
};

// SpanningVertexListDescriptor — VertexList with an array of
// PersistentObjects + multiple per-object channel groups + trailer.
// Body from NR2003 0x005e2250:
//   VertexList parent (u32 magic_parent == 1)
//   u32 magic (== 1)
//   u32 max_vertices @ this+0x70  (cap 0x4098)
//   u32 num_objects  @ this+0x74
//   transferPointerButNotData(this+0x78, num_objects*4)  -- object-ptr array
//   for each i: transferPersistentObject(&objects[i])
//   transferPointerButNotData(this+0x7c, num_objects*4)  -- channel-A ptr array
//   for each i: transferPointer(channel_a[i], max_vertices*8)
//   4 × transferPointerButNotData(this+0x80..+0x8c, num_objects*4)
//   for each i: 4 × transferPointer(channel[i], max_vertices*8)  -- group B
//   2 × transferPointerButNotData(this+0x90, +0x94, num_objects*4)
//   1 × transferPointerButNotData(this+0x98, num_objects*4)
//   for each i: 3 × transferPointer(channel[i], max_vertices*8)  -- group C
//   FUN_005e0c40 trailer with max_vertices
class SpanningVertexListDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "SpanningVertexListDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t magic_parent = 0;
    std::uint32_t magic = 0;
    std::uint32_t max_vertices = 0;
    std::uint32_t num_objects = 0;

    std::vector<std::shared_ptr<PersistentObject>> objects;
    // Per-object channel data: channels_a[i] = max_vertices doubles.
    std::vector<std::vector<double>> channels_a;
    // Group B: 4 channels × num_objects.  channels_b[c][i] = max_vertices doubles.
    std::vector<std::vector<double>> channels_b[4];
    // Group C: 3 channels × num_objects.
    std::vector<std::vector<double>> channels_c[3];

    std::uint32_t trailer_magic = 0;
    std::vector<std::vector<double>> trailer_channels;

protected:
    void read_body(Archive& ar) override;
};

// X_SectionDescriptor — track cross-section data.  Body from NR2003
// 0x005e6370.  All scalar fields use the version-conditional pattern:
// versions <= 2 wrote f64s; versions >= 3 write f32s.  The reader accepts
// all native versions 0..3 and performs the same f64-to-f32 narrowing.
//
//   universal_header
//   u32 magic                                @ stack, accepted <= 3
//   f32 lateral_start                        @ +0x0c
//   f64 height_start                         @ +0x15
//   f32 slope_start                          @ +0x1d
//   f32 lateral_end                          @ +0x10
//   f64 height_end                           @ +0x25
//   if height_end <= MAX_DOUBLE_BOUND:
//     f32 slope_end                          @ +0x2d
//     [v >= 2]: u8 visual_curve_mode         @ +0x14
//     [v >= 3]:
//       endpoint seam metadata for start, then end. Kind 0 is ordinary,
//       kind 1 is a hard discontinuity, and kind 2 is an explicit stitch.
//       A kind-2 payload stores two f32 seam parameters plus source/target
//       cross-section boundary indices. The adjacent segment mirrors an
//       explicit stitch. This is authoring/export topology; the stock
//       physics and renderer compilers do not consume it.
//
// All f32 reads in the FULL path are validated NaN-free and within
// |x| < |bound|.  We perform NaN checks but skip the bound checks (the
// bounds are runtime constants we don't have surfaced).
class X_SectionDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "X_SectionDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t magic = 0;
    float  lateral_start = 0;
    double height_start = 0;
    float  slope_start = 0;
    float  lateral_end = 0;
    double height_end = 0;
    float  slope_end = 0;
    std::uint8_t visual_curve_mode = 0; // 0 follows arc; nonzero uses endpoint chord

    struct EndpointSeamMetadata {
        std::uint8_t kind = 0;
        float parameter0 = 0, parameter1 = 0;
        std::uint8_t source_boundary_index = 0;
        std::uint8_t target_boundary_index = 0;
        bool hard_discontinuity() const noexcept { return kind == 1; }
        bool explicit_stitch() const noexcept { return kind == 2; }
    };
    EndpointSeamMetadata start_seam;
    EndpointSeamMetadata end_seam;
    bool complete_payload_read = false;

protected:
    void read_body(Archive& ar) override;
};

// F_SectionDescriptor — track flat-section data.  Body from NR2003
// 0x005e6a50:
//   universal_header
//   u32 magic            (accepted <= 5)
//   f32 lateral_start    (v<5 was f64)
//   f32 lateral_end      (v<5 was f64)
//   u8  boundary_mode    (v<3: default 0; 0=parametric, 1=world chord)
//   u32 query_tag        (v<4: default 1; passed through track queries)
//   u32 surface_code     (always; normalized by the native material mapper)
//   transferPersistentObject(&this+0x1d)   -- X_Section reference
//   if X_Section is non-empty (any of *(X+0xc, +0x10, +0x14, +0x18, +0x20) != 0):
//     transferPersistentObject(&this+0x21)  -- W_Section reference
//     // (runtime side-effects on the loaded sections — skipped in reader)
//
// Magic sentinel: when iStack_24 == 1 in write mode we early-return.
// We don't honor that since we're read-only.
class F_SectionDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "F_SectionDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t magic = 0;
    float         lateral_start = 0;
    float         lateral_end = 0;
    std::uint8_t  boundary_mode = 0;
    std::uint32_t query_tag = 0;
    std::uint32_t surface_code = 0;
    std::shared_ptr<PersistentObject> x_section;   // @ +0x1d
    std::shared_ptr<PersistentObject> w_section;   // @ +0x21 (conditional)

    // Per-slot trailer block read by NR2003.exe FUN_005e69a0 (the
    // F-section trailer helper).  Six slots, indexed by the X-field
    // offset whose value gates the block:
    //
    //   slot | call order | X-field offset | X-Section field            | Appearance field
    //   ---- | ---------- | -------------- | -------------------------- | -----------------
    //   0    | 1st        | +0xc           | left_edge_lateral (f32)    | texture_slots[0]
    //   1    | 2nd        | +0x10          | mid_offset (f32)           | texture_slots[1]
    //   2    | 3rd        | +0x18          | mid bytes of height (f64)  | texture_slots[3]
    //   3    | 4th        | +0x14          | flag_v2 + low height bytes | texture_slots[2]
    //   4    | 5th        | +0x1c          | end of height + start of   | texture_slots[4]
    //        |            |                | right_edge_lateral         |
    //   5    | 6th        | +0x20          | end of right_edge_lateral  | texture_slots[5]
    //
    // Per-slot block layout (struct offsets relative to F_Section base):
    //
    //   +0x25 + slot      : gate u8       (omitted iff X-field is 0)
    //   +0x2b + slot*2    : 2 × u8 flags  (full block only)
    //   +0x37 + slot*0x10 : 2 × f64       (full block only — small-range
    //                                       "pair_f64", empirically
    //                                       0.008..0.6 on shipped tracks)
    //   +0x97 + slot*0x18 : 3 × f64       (full block only — wider-range
    //                                       "triple_f64", empirically
    //                                       −65..+20 on shipped tracks)
    //
    // Full block = 1 + 2 + 16 + 24 = 43 bytes on disk.
    // The in-memory struct layout is the SAME byte layout as the wire
    // format — confirmed by F_Section's copy function FUN_005e68d0 which
    // mempcpy's 18 + 96 + 144 = 258 trailer bytes verbatim.
    //
    // Negative consumer closure: the native runtime parses and copy-constructs
    // these six per-anchor blocks but neither its physics compiler nor the
    // renderer consumes them. Their value ranges and association with the six
    // Appearance texture slots identify authoring/export UV anchor state.
    // Preserve the exact flag/pair/triple structure and authored ordering.
    //
    //   pair_f64   — small authoring rates associated with an anchor.
    //   triple_f64 — signed authoring offsets associated with an anchor.
    //   flags_u8   — per-anchor export/interpolation controls.
    //
    // Do not reinterpret this runtime-inert payload as surface geometry or
    // material physics.
    struct TrailerBlock {
        bool         x_field_nonzero = false;
        std::uint8_t gate = 0;
        bool         full = false;        // gate==0 path: full 42-byte payload follows
        std::uint8_t flags_u8[2] = {};    // 2 × u8 at +0x2b + slot*2
        double       pair_f64[2] = {};    // 2 × f64 at +0x37 + slot*0x10
        double       triple_f64[3] = {};  // 3 × f64 at +0x97 + slot*0x18
    };
    std::array<TrailerBlock, 6> trailer_blocks{};

protected:
    void read_body(Archive& ar) override;
};

// W_SectionDescriptor — track wall/wide-section data.  Body from
// NR2003 0x005e7330 (1765 B).  Structure (decoded from raw disasm):
//   universal_header
//   u32 magic            (accepted <= 7)
//   f32 lateral_start    (v<6 was f64)
//   f32 lateral_end      (v<6 was f64; this is the "+0x08 in unsigned"
//                          slot in the disasm — at this+0x08 from the
//                          casted base pointer, which from our universal
//                          header view starts after the header)
//   u8  boundary_mode    (v<3: default 0; 0=parametric, 1=world chord)
//   u32 query_tag        (v<4: default 1)
//   f64 height_start/end
//   f64 visual_face_offset_start/end
//   f64 collision_half_thickness_start/end
//   i32 height_offset_mode (0=world vertical, nonzero=bank-normal; -1 invalid)
//   u32 wall_profile_kind  (v<7: default 0)
//   if wall_profile_kind == 1:
//     transferBytes(&cubic_profile, 0x28)    @ this+0x4d (allocated)
//   u32 surface_code     (always)
//   u32 longitudinal_record_count (v<5: default 1)
//   for each record:
//     [v >= 5]: 8 bytes (2 u32 OR f64) @ record+0x28
//     for each j in [0..2):
//       transferPersistentObject(&record[j])
//       if obj is non-empty (children fields):
//         transferPersistentObject(&record[j+1])
//         (W-section runtime side-effects; skipped in reader)
//         + per-record metadata bytes
//
// Versions 0..7 are accepted.  The pre-v6 f64 coordinates and the defaults
// for fields introduced in v3/v4/v5/v7 mirror the native reader.
class W_SectionDescriptor : public DescriptorBase {
public:
    static constexpr const char* kClassName = "W_SectionDescriptor";
    std::string_view class_name() const override { return kClassName; }

    std::uint32_t magic = 0;
    float lateral_start = 0;
    float lateral_end = 0;
    std::uint8_t boundary_mode = 0;
    std::uint32_t query_tag = 0;
    double height_start = 0, height_end = 0;
    double visual_face_offset_start = 0, visual_face_offset_end = 0;
    double collision_half_thickness_start = 0, collision_half_thickness_end = 0;
    std::int32_t height_offset_mode = -1;
    std::uint32_t wall_profile_kind = 0;
    std::vector<std::uint8_t> cubic_profile; // 10 f32 when wall_profile_kind == 1
    std::uint32_t surface_code = 0;
    std::uint32_t longitudinal_record_count = 0;
    // Per-record: NR2003's W_Section record is a 0x53a-byte struct that
    // contains an INNER 5-iteration loop. Each pair binds typed authoring
    // state for a wall face/transition and carries six optional export
    // interpolation blocks gated by fields of child_a.
    struct NestedInterpolationEntry {
        std::uint8_t flag = 0;
        double rate = 0;
        double offset = 0;
    };
    struct NestedInterpolationBlock {
        bool source_field_nonzero = false;
        std::uint8_t gate = 0;
        bool full = false;
        std::array<NestedInterpolationEntry, 2> entries{};
    };
    struct InnerPair {
        std::shared_ptr<PersistentObject> child_a;
        std::shared_ptr<PersistentObject> child_b;
        std::array<NestedInterpolationBlock, 6> interpolation_blocks{};
    };
    struct Record {
        // Normalized longitudinal range start; record zero is always 0.
        double longitudinal_position = 0.0;
        InnerPair inner[5];
    };
    std::vector<Record> records;

protected:
    void read_body(Archive& ar) override;
};

// =========================================================================
// All previously-PARTIAL classes are now FULL.  No PARTIAL tier remains.
// =========================================================================

// =========================================================================
// Registration
// =========================================================================
//
// Static-init macros aren't reliable across static-lib boundaries (the
// linker can drop TUs with no referenced symbols).  Call this once
// before opening an Archive that needs to dispatch by class name.
// Idempotent.
void register_all_descriptors();

}  // namespace opennr::papyrus
