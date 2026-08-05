#include "object_stream.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace opennr {

namespace {

const std::unordered_set<std::string_view>& known_classes() {
    static const std::unordered_set<std::string_view> s = {
        // scene graph
        "EmptyDescriptor", "NodeDescriptor", "ChildNodeDescriptor",
        "GroupingNodeDescriptor", "GroupDescriptor", "LodSwitchDescriptor",
        "StateSwitchDescriptor", "TransformDescriptor",
        "AnimatedTransformDescriptor", "BillboardDescriptor",
        "SelfLightingDescriptor", "AppNodeDescriptor", "PortalDescriptor",
        // geometry
        "ShapeDescriptor", "GeometryDescriptor",
        "TriStripDescriptor", "TriFanDescriptor", "TriListDescriptor",
        "BiCubicPatchDescriptor",
        // vertex data
        "VertexListDescriptor", "PlainVertexListDescriptor",
        "MorphVertexListDescriptor", "RegionMorphVertexListDescriptor",
        "LodMorphVertexListDescriptor", "StateMorphVertexListDescriptor",
        "SpanningVertexListDescriptor",
        "ProgressiveModificationDescriptor", "ProgressiveMeshDescriptor",
        // material / texture / lighting
        "TextureDescriptor", "TextureCoordsDescriptor",
        "AppearanceDescriptor",
        "PointLightDescriptor", "InfiniteLightDescriptor",
        // track-only
        "TrackDescriptor", "TrackDetailDescriptor",
        "TSODescriptor", "TSOReferenceDescriptor",
        "SegmentDescriptor",
        "X_SectionDescriptor", "F_SectionDescriptor", "W_SectionDescriptor",
        "TrackGrooveDescriptor", "TrackRaceLineDescriptor",
    };
    return s;
}

bool is_plausible_token(std::span<const std::uint8_t> bytes, std::size_t pos,
                         std::uint32_t n) {
    if (n < 2 || n > 64) return false;
    if (pos + 4 + n > bytes.size()) return false;
    auto first = bytes[pos + 4];
    auto last  = bytes[pos + 4 + n - 1];
    if (last != 0) return false;            // must be NUL-terminated
    if (first == 0) return false;            // can't start with NUL
    for (std::uint32_t i = 0; i < n - 1; ++i) {
        std::uint8_t b = bytes[pos + 4 + i];
        if (b < 0x20 || b >= 0x7F) return false;
    }
    return true;
}

}  // namespace

bool is_known_object_class(std::string_view name) {
    return known_classes().contains(name);
}

ObjectStream ObjectStream::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 8) {
        throw std::runtime_error("ObjectStream: file shorter than 8-byte header");
    }
    ObjectStream s;
    auto read_u32 = [&](std::size_t pos) {
        return static_cast<std::uint32_t>(bytes[pos]) |
               (static_cast<std::uint32_t>(bytes[pos + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes[pos + 2]) << 16) |
               (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
    };
    s.stream_version_a = read_u32(0);
    s.stream_version_b = read_u32(4);

    std::size_t pos = 0;
    while (pos + 4 <= bytes.size()) {
        std::uint32_t n = read_u32(pos);
        if (is_plausible_token(bytes, pos, n)) {
            std::string name(reinterpret_cast<const char*>(&bytes[pos + 4]), n - 1);
            // Filter out random ASCII matches that aren't identifier-shaped.
            bool ok =
                name[0] == '_' || name[0] == '(' ||
                std::isalpha(static_cast<unsigned char>(name[0])) ||
                name.find('.') != std::string::npos ||
                name.find('_') != std::string::npos;
            if (ok) {
                ObjectToken tok;
                tok.offset = pos;
                tok.length = n;
                tok.name   = std::move(name);
                tok.is_class = is_known_object_class(tok.name);
                s.tokens.push_back(std::move(tok));
                pos += 4 + n;
                continue;
            }
        }
        ++pos;
    }
    return s;
}

std::vector<std::string> ObjectStream::texture_refs() const {
    std::vector<std::string> out;
    for (const auto& t : tokens) {
        if (!t.is_class && t.name.size() >= 4) {
            auto n = t.name.size();
            if (t.name.compare(n - 4, 4, ".mip") == 0) {
                out.push_back(t.name);
            }
        }
    }
    return out;
}

std::vector<std::string> ObjectStream::object_refs() const {
    std::vector<std::string> out;
    for (const auto& t : tokens) {
        if (!t.is_class && t.name.size() >= 4) {
            auto n = t.name.size();
            if (t.name.compare(n - 4, 4, ".3do") == 0) {
                out.push_back(t.name);
            }
        }
    }
    return out;
}

std::vector<std::string> ObjectStream::classes_in_order() const {
    std::vector<std::string> out;
    for (const auto& t : tokens) {
        if (t.is_class) out.push_back(t.name);
    }
    return out;
}

bool ObjectStream::has_known_root() const {
    if (tokens.empty()) return false;
    return tokens.front().is_class;
}

}  // namespace opennr
