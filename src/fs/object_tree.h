#pragma once

// Recursive parser for the Papyrus typed-stream `.3do` / `.ptf`
// format.  This is the next step up from `object_stream.{h,cpp}` —
// where that file just scans the byte stream for class-name tokens
// (and works against all 1,803 shipped files), this file actually
// builds a typed tree by walking each class body's fields.
//
// Coverage is best-effort:
//
//   * Class names are always identified (same NUL-terminated ASCII
//     token scan as the structural walker).
//   * For classes whose body layout is documented in
//     `docs/formats/3do_descriptor_*.md`, the body is decoded into a
//     typed payload.
//   * For unknown bodies, the parser SKIPS bytes until the next
//     class-name token shows up — losing nothing the structural
//     walker doesn't already lose, but never desynchronising.
//
// The output is therefore always a valid token-aligned tree; the
// payload-richness scales with how many class decoders we've
// implemented.  This is fine for first-pass rendering of `.ptf`
// tracks (which only need TrackDescriptor + SegmentDescriptor counts
// to scaffold a synthetic surface) and for cataloguing what's inside
// a `.3do` (texture refs, sub-object refs, primitive-class counts).

#include "object_stream.h"
#include "descriptors.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace opennr {

// ---- Legacy payloads -------------------------------------------------
//
// The original object_tree decoder shipped with a small set of "light"
// payload structs.  These are kept (with the same names) so existing
// callers and tests continue to compile.  New code should use the
// strongly-typed descriptors in `descriptors.h` (accessed via the
// `descriptor` field on each node).

struct GroupPayload {
    std::uint32_t num_children = 0;
};

struct TrackPayload {
    std::uint32_t num_segments = 0;
};

struct TransformPayload {
    double tx = 0, ty = 0, tz = 0;
    double yaw = 0, pitch = 0, roll = 0;
};

struct LodSwitchPayload {
    std::int32_t num_levels = 0;
    double       centre_x = 0, centre_y = 0, centre_z = 0;
};

struct TexturePayload {
    std::string name;     // e.g. "series_flagger.mip"
};

// ---- Strongly-typed payload variant ---------------------------------

using DescriptorPayload = std::variant<
    std::monostate,
    GroupDescriptor,
    GroupingNodeDescriptor,
    LodSwitchDescriptor,
    StateSwitchDescriptor,
    TransformDescriptor,
    AnimatedTransformDescriptor,
    BillboardDescriptor,
    PortalDescriptor,
    PointLightDescriptor,
    AppearanceDescriptor,
    ProgressiveModificationDescriptor,
    TrackDescriptor,
    SegmentDescriptor,
    X_SectionDescriptor,
    F_SectionDescriptor,
    W_SectionDescriptor,
    TSODescriptor,
    TSOReferenceDescriptor,
    TrackDetailDescriptor,
    TextureCoordsDescriptor,
    GeometryDescriptor,
    ShapeDescriptor,
    TextureDescriptor,
    PlainVertexListDescriptor,
    TriStripDescriptor,
    TriListDescriptor,
    TriFanDescriptor>;

// ---- Tree node -------------------------------------------------------

struct ObjectNode;
using ObjectNodePtr = std::shared_ptr<ObjectNode>;

struct ObjectNode {
    std::string                                    class_name;
    std::size_t                                    body_offset = 0;
    std::size_t                                    body_length = 0;  // 0 if unknown
    std::vector<ObjectNodePtr>                     children;

    // Optional decoded payload, present iff the parser knew this class.
    // Legacy variant; kept for backward compatibility.
    std::variant<std::monostate,
                 GroupPayload,
                 TrackPayload,
                 TransformPayload,
                 LodSwitchPayload,
                 TexturePayload>                   payload;

    // Strongly-typed payload variant covering all decoded descriptors
    // (see `descriptors.h`).  Both `payload` and `descriptor` are
    // populated when a class is known.
    DescriptorPayload                              descriptor;

    template <class T>
    const T* as() const { return std::get_if<T>(&payload); }

    template <class T>
    const T* desc() const { return std::get_if<T>(&descriptor); }
};

// ---- Top-level parse result ------------------------------------------

struct ObjectTree {
    std::uint32_t stream_version_a = 0;
    std::uint32_t stream_version_b = 0;
    ObjectNodePtr root;
    // Flat per-class count for quick lookups (e.g. "how many
    // SegmentDescriptor instances in atlanta.ptf?").
    std::unordered_map<std::string, std::uint32_t> class_counts;

    // Returns nullptr when the file is empty / no root class found.
    const ObjectNode* find_first(std::string_view class_name) const;

    static ObjectTree parse(std::span<const std::uint8_t> bytes);
};

}  // namespace opennr
