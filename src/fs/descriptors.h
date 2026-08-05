#pragma once

// Strongly-typed payloads for the per-class bodies in `.3do` / `.ptf`
// typed-stream files (see `docs/formats/3do_object_stream.md`,
// `docs/formats/3do_descriptor_layouts.md`,
// `docs/formats/3do_descriptor_fields.md`).
//
// On-disk body layout (recovered from shipped Atlanta `.3do` + `.ptf`):
//
//   u32   version          observed always = 1
//   u32   name_length      length of the optional node-name string,
//                            including the trailing NUL.  0 if absent.
//   char  name[name_length] (only present if name_length > 0;
//                            ends in a NUL byte)
//   ...   class-specific fields
//
// Note: this is NOT the runtime in-memory layout documented in
// `3do_descriptor_layouts.md` -- that doc describes the C++ object as it
// lives in the engine, including the vtable and base-class fields.  The
// on-disk form is the serialization produced by Papyrus's `Archive`
// framework.  For most descriptors the disk form contains a strict
// subset of the runtime fields, in a different order, with name-tagged
// scalars rather than positional.  Where the disk form matches the
// runtime layout, the layout doc remains correct; where it differs the
// per-decoder comments below note the discrepancy.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace opennr {

// Common header carried by every descriptor body.
struct DescriptorHeader {
    std::uint32_t version       = 0;  // observed = 1
    std::uint32_t name_length   = 0;  // 0 = no name
    std::string   name;               // empty when name_length == 0
    std::size_t   header_bytes  = 0;  // size of the header on disk
                                      //   (8 + name_length)
};

// ---- Per-descriptor payloads (one struct per known class) ------------
//
// Every payload starts with the `DescriptorHeader` above.  Field
// additions go AFTER the header, in disk order.

struct GroupDescriptor {
    DescriptorHeader header;
    // Disk layout after the header: a 4-byte flag (observed = 1), a
    // 4-byte sub-type (observed = 1), 48 bytes of zeros (presumed
    // 6-double bounding box / sphere), then an unaligned child run
    // we don't yet pin field-by-field.  We expose the only field we
    // verified across many files: the apparent num_children candidate
    // that immediately follows the bbox region.
    std::uint32_t flag_a       = 0;
    std::uint32_t flag_b       = 0;
    std::uint32_t num_children = 0;
};

struct GroupingNodeDescriptor {
    DescriptorHeader header;
    std::uint32_t flag_a       = 0;
    std::uint32_t flag_b       = 0;
    std::uint32_t num_children = 0;
};

struct LodSwitchDescriptor {
    DescriptorHeader header;
    // From the runtime layout (`3do_descriptor_layouts.md`):
    //   num_lod_levels   (i32)
    //   centre_x/y/z     (3 doubles)
    //   per-level switch distances (doubles)
    // Disk-side we observe num_lod_levels as the first u32 after the
    // header.
    std::int32_t  num_lod_levels = 0;
};

struct StateSwitchDescriptor {
    DescriptorHeader header;
    std::int32_t  num_states = 0;
};

struct TransformDescriptor {
    DescriptorHeader header;
    // Disk form (confirmed across 9+ shipped files):
    //   universal_header  (version=1, name_length=N, name)
    //   u32 flag_a = 1
    //   u32 flag_b = 1
    //   48 bytes of zeros (presumed initial bbox / 6 zero doubles)
    //   u32 marker = 1
    //   6 doubles UNALIGNED: tx, ty, tz, yaw, pitch, roll
    // The 6 doubles match the runtime layout from `dump()` @ 005D9730.
    double tx = 0.0, ty = 0.0, tz = 0.0;
    double yaw = 0.0, pitch = 0.0, roll = 0.0;
    bool   fields_decoded = false;     // true if we recovered all 6
};

struct AnimatedTransformDescriptor {
    DescriptorHeader header;
    double tx = 0.0, ty = 0.0, tz = 0.0;
    double axis_x = 0.0, axis_y = 0.0, axis_z = 0.0;
    bool   fields_decoded = false;
};

struct BillboardDescriptor {
    DescriptorHeader header;
    // Typed-stream body: Transformable parent, then pivot/translation XYZ,
    // facing-axis XYZ, and a child object. The token scanner does not walk
    // the child, so use papyrus::BillboardDescriptor for full decoding.
    double pivot_x = 0, pivot_y = 0, pivot_z = 0;
    double axis_x = 0, axis_y = 0, axis_z = 0;
    bool   fields_decoded = false;
};

struct PortalDescriptor {
    DescriptorHeader header;
    // Typed-stream body contains a target object followed by a u32 index
    // array. It has no serialized colour or centre fields.
    std::uint32_t num_indices = 0;
    std::vector<std::uint32_t> indices;
    bool   fields_decoded = false;
};

struct PointLightDescriptor {
    DescriptorHeader header;
    // Runtime form has 3 bools then 9 unaligned doubles.  Disk form
    // not pinned across shipped files; placeholders kept.
    bool   diffuse_set = false, specular_set = false, ambient_set = false;
    double diffuse_r = 0, diffuse_g = 0, diffuse_b = 0;
    double specular_r = 0, specular_g = 0, specular_b = 0;
    double ambient_r = 0, ambient_g = 0, ambient_b = 0;
    bool   fields_decoded = false;
};

struct AppearanceDescriptor {
    DescriptorHeader header;
    // From the trace strings (`3do_descriptor_fields.md`):
    //   ambient r,g,b   (3 doubles)
    //   diffuse r,g,b
    //   specular r,g,b
    //   shinyness, reflectivity, opacity   (3 doubles)
    //   envmap_index, EMBM_TexCoordScale, EMBM_NormalScale,
    //     EMBM_LuminosityScale  (4 floats)
    //   forced_bumpmap  (i32 or u8)
    // Disk form interleaves inline TextureDescriptor children for each
    // of the 7 texture cascade slots.  We don't yet pin all of this
    // across shipped files -- the fields below are populated when we
    // recover them by best-effort scanning; `fields_decoded` says
    // whether we trust the numeric values.
    double ambient_r = 0, ambient_g = 0, ambient_b = 0;
    double diffuse_r = 0, diffuse_g = 0, diffuse_b = 0;
    double specular_r = 0, specular_g = 0, specular_b = 0;
    double shinyness = 0, reflectivity = 0, opacity = 0;
    float  envmap_index = 0;
    float  embm_texcoord_scale = 0;
    float  embm_normal_scale = 0;
    float  embm_luminosity_scale = 0;
    std::int32_t forced_bumpmap = 0;
    bool   fields_decoded = false;
};

struct ProgressiveModificationDescriptor {
    DescriptorHeader header;
    std::int32_t change_num_vertices   = 0;
    std::int32_t num_modified_vertices = 0;
    std::int32_t change_num_tris       = 0;
    std::int32_t num_modified_tris     = 0;
    bool         fields_decoded        = false;
};

struct TrackDescriptor {
    DescriptorHeader header;
    // For all 26 shipped `.ptf` files the body starts with:
    //   u32 version=1, u32 name_length=0, u32 type=8, u32 num_segments
    // (atlanta = 37, etc.).
    //
    // Disassembly of TrackDescriptor::read() @ 0x005e52c0 shows
    // additional doubles (3 scale factors + various track-wide params)
    // appear ONLY when the typed-stream's per-class version >= 3.
    // Shipped files (version 2) skip them; the defaults are
    // 0x3FC99999 in the high u32 of each double, i.e. 0.2.
    std::uint32_t type_code      = 0;
    std::int32_t  num_segments   = 0;
};

// SegmentDescriptor — one track segment.  Disk layout recovered from
// SegmentDescriptor::read() @ 0x005e5b40:
//
//   universal_header  (version=1, name_length=0)
//   u32 type_code     (== 2)
//   u32 num_x_section_refs
//   u32 x_section_refs[num_x_section_refs]  (typed-stream object IDs)
//   u32 num_f_section_refs
//   u32 f_section_refs[num_f_section_refs]
//   u32 num_w_section_refs
//   u32 w_section_refs[num_w_section_refs]
//   i32 segment_kind                   (-1 sentinel skips the rest)
//   [if segment_kind != -1]:
//     double pos_a   double pos_b      (probably segment endpoint /
//                                        anchor position; validated
//                                        against trig of pos_c)
//     double angle_c                   (validated as sin/cos-finite)
//     double pos_d   double pos_e
//     double angle_f                   (also trig-validated)
//     u8 flag_a   u8 flag_b
//     u32 num_records_12   (count of 12-byte records)
//     byte[num_records_12 * 12]        (probably 3 floats / record)
//     u32 num_records_16
//     byte[num_records_16 * 16]        (probably 4 floats / record)
struct SegmentDescriptor {
    DescriptorHeader header;
    std::uint32_t              type_code = 0;
    std::vector<std::uint32_t> x_section_refs;
    std::vector<std::uint32_t> f_section_refs;
    std::vector<std::uint32_t> w_section_refs;
    std::int32_t               segment_kind   = -1;
    // Position + orientation triple; valid only when segment_kind != -1.
    double                     pos_a   = 0.0;
    double                     pos_b   = 0.0;
    double                     angle_c = 0.0;
    double                     pos_d   = 0.0;
    double                     pos_e   = 0.0;
    double                     angle_f = 0.0;
    std::uint8_t               flag_a  = 0;
    std::uint8_t               flag_b  = 0;
    // Surface-detail controls. The 12-byte rows select a TrackDescriptor
    // amplitude/distance source; the 16-byte rows supply a lateral boundary
    // pair and a multiplicative height-scale pair.
    std::vector<std::uint8_t>  records_12_raw;  // num_records_12 * 12 bytes
    std::vector<std::uint8_t>  records_16_raw;  // num_records_16 * 16 bytes
    bool                       fields_decoded = false;
};

// X_SectionDescriptor — cross-section of a segment at a station.
// Disk layout recovered from X_SectionDescriptor::read() @ 0x005e6370.
// Note: pre-v3 stream version writes the fields as DOUBLES; v3+ writes
// them as FLOATS.  Runtime stores all as float.
struct X_SectionDescriptor {
    DescriptorHeader header;
    std::uint32_t  type_code = 0;       // == 3
    float          lateral_start = 0.0f;
    double         height_start = 0.0;
    float          slope_start = 0.0f;
    float          lateral_end = 0.0f;
    double         height_end = 0.0;
    float          slope_end = 0.0f;
    std::uint8_t   visual_curve_mode = 0;
    struct EndpointSeam {
        std::uint8_t kind = 0;
        float parameter0 = 0.0f;
        float parameter1 = 0.0f;
        std::uint8_t source_boundary_index = 0;
        std::uint8_t target_boundary_index = 0;
    };
    EndpointSeam start_seam;
    EndpointSeam end_seam;
    bool           fields_decoded = false;
};

// The lightweight token-tree view only surfaces F/W headers. The canonical
// typed Archive parser in papyrus_descriptors.h decodes their complete bodies.
struct F_SectionDescriptor {
    DescriptorHeader header;
};

// W_SectionDescriptor — wall section (barriers, SAFER, retaining walls).
struct W_SectionDescriptor {
    DescriptorHeader header;
};

// TSODescriptor — TrackSide Object definition (one entry per
// in-world prop type).  25 bytes in memory; carries a name and a
// reference to a .3do mesh file in the typed-stream graph.
struct TSODescriptor {
    DescriptorHeader header;
};

// TSOReferenceDescriptor — placement instance of a TSO into the
// world.  84 bytes in memory: a transform + a TSO back-reference.
struct TSOReferenceDescriptor {
    DescriptorHeader header;
};

// TrackDetailDescriptor — per-segment surface detail (grass, kerbs,
// painted markings).  64 bytes in memory.
struct TrackDetailDescriptor {
    DescriptorHeader header;
};

// TextureCoordsDescriptor — per-stage UV channel data, used by the
// track texture cascade.  116 bytes in memory.
struct TextureCoordsDescriptor {
    DescriptorHeader header;
};

struct GeometryDescriptor {
    DescriptorHeader header;
    // Observed disk form (universal across shipped files):
    //   u32 type      (observed = 1)
    //   u32 unk_a     (observed = 11)
    //   u32 unk_b     (observed = 12)
    // followed by an inline VertexList* child + one or more primitive
    // descriptor children.  Field meanings beyond `type` are unknown.
    std::uint32_t type_code = 0;
    std::uint32_t unk_a     = 0;
    std::uint32_t unk_b     = 0;
};

struct ShapeDescriptor {
    DescriptorHeader header;
    // Shape carries a name (e.g. "Box01", "tree_01") that the engine
    // uses for state-switch and progressive-mesh lookups.  Body fields
    // after the header are not yet pinned across files.
};

struct TextureDescriptor {
    DescriptorHeader header;
    // Disk layout after the header:
    //   u32 type           (observed = 2)
    //   u32 tex_name_len   (length INCLUDING the NUL)
    //   char tex_name[len]
    //   25 bytes (purpose unclear -- often all zero)
    //   12 doubles (unaligned)  -- UV transform; the identity is
    //                              9*1.0 followed by (0, 0.05, 1.0)
    std::uint32_t type_code      = 0;
    std::string   texture_name;          // e.g. "series_flagger.mip"
    std::vector<double> uv_matrix;       // size = 12 when decoded
};

struct PlainVertexListDescriptor {
    DescriptorHeader header;
    // Disk form confirmed across 232 PVLs from samples and atlanta DAT:
    //   universal_header                          (version=1, name_length=0)
    //   u32 type_code = 1
    //   u32 flag = 1
    //   u32 num_vertices
    //   u32 size = num_vertices*8, double pos_x[num_vertices]
    //   u32 size = num_vertices*8, double pos_y[num_vertices]
    //   u32 size = num_vertices*8, double pos_z[num_vertices]
    //   u32 separator = 0
    //   u32 size = num_vertices*8, double normal_x[num_vertices]
    //   u32 size = num_vertices*8, double normal_y[num_vertices]
    //   u32 size = num_vertices*8, double normal_z[num_vertices]
    //   u32 num_uv_channels
    //   per UV channel:  u32 size  (0 if empty), double values[num_vertices]
    //   trailer of u32 size fields (usually mostly zeros; not fully decoded).
    //
    // Normals are unit-length on 1003/1003 PVLs sampled.  UV values fall
    // in [0, 1] when present.
    std::int32_t                num_vertices = 0;
    std::vector<double>         positions_x;
    std::vector<double>         positions_y;
    std::vector<double>         positions_z;
    std::vector<double>         normals_x;
    std::vector<double>         normals_y;
    std::vector<double>         normals_z;
    // UV channels: outer vector is per-channel (typically 2 used: U then V),
    // inner vector is per-vertex.  Empty channels (size=0 on disk) are kept
    // as empty inner vectors.
    std::vector<std::vector<double>>   uv_channels;
};

// TriList simple form (verified on 167/167 sample+atlanta_dat files):
//   universal_header                          (version=1, name_length=0)
//   u32 type_code = 1
//   u32 flag_a = 0
//   u32 flag_b = 1
//   For each sublist (until body end):
//     u32 num_indices
//     u32 size_bytes = num_indices * 4
//     u32 indices[num_indices]
//     IF more bytes follow: u32 marker = 1 (start of next sublist)
//
// A small minority (gas_man.3do, etc.) carry embedded named state-frames
// instead of a clean index array — those bodies look like animated
// state-switch data and aren't fully decoded here.
struct TriListSublist {
    std::int32_t              num_indices = 0;
    std::vector<std::uint32_t> indices;
};

struct TriListDescriptor {
    DescriptorHeader            header;
    std::uint32_t                type_code = 0;
    std::uint32_t                flag_a    = 0;
    std::uint32_t                flag_b    = 0;
    std::vector<TriListSublist>  sublists;
    bool                          fields_decoded = false;
    // Legacy: total of num_indices across sublists for back-compat.
    std::int32_t                  num_indices = 0;
};

// TriStrip and TriFan have a different body shape:
//   universal_header (version=1, name_length=0)
//   u32 type_code = 1
//   For each strip-segment (5 u32s, 20 bytes):
//     u32 idx_a
//     u32 idx_b
//     u32 sub_version  = 1
//     u32 sub_name_len = 0
//     u32 sub_type     = 1
// The semantic of (idx_a, idx_b) — whether it's a (next_index, base_index)
// strip walk or an edge list — is not yet pinned across files.  We expose
// the records as-is.
struct StripSegment {
    std::uint32_t idx_a = 0;
    std::uint32_t idx_b = 0;
};

struct TriStripDescriptor {
    DescriptorHeader            header;
    std::uint32_t                type_code = 0;
    std::vector<StripSegment>    segments;
    bool                          fields_decoded = false;
    // Legacy: number of segments (= number of (a, b) pairs).
    std::int32_t                  num_indices = 0;
};

struct TriFanDescriptor {
    DescriptorHeader            header;
    std::uint32_t                type_code = 0;
    std::vector<StripSegment>    segments;
    bool                          fields_decoded = false;
    std::int32_t                  num_indices = 0;
};

// ---- Universal header decoder ---------------------------------------

// Decode the 8-byte universal header (version + name_length) plus the
// optional name string.  `body_pos` must point at the first byte of the
// descriptor body (i.e. just past the class-name token's NUL).
// Returns the header; `header_bytes` reports the on-disk size consumed
// so the caller can advance to class-specific fields.
DescriptorHeader decode_descriptor_header(
    std::span<const std::uint8_t> bytes, std::size_t body_pos);

}  // namespace opennr
