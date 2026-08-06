#pragma once

#include "math/vec.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace opennr::viewer {

enum class TrackViewMode : std::uint32_t {
    Solid = 1u,
    Wireframe = 2u,
    Collision = 4u,
};

using TrackViewModes = std::uint32_t;

constexpr TrackViewModes track_view_mode(TrackViewMode mode) {
    return static_cast<TrackViewModes>(mode);
}

constexpr bool has_track_view_mode(TrackViewModes modes, TrackViewMode mode) {
    return (modes & track_view_mode(mode)) != 0;
}

struct TrackTriangle {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::uint32_t color = 0;
    std::uint32_t surface_code = 0;
    std::int32_t texture = -1;
};

struct TrackVertex {
    Vec3 position{};
    float u = 0.0f;
    float v = 0.0f;
};

struct TrackLine {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

struct TrackMesh {
    std::vector<TrackVertex> vertices;
    std::vector<TrackTriangle> triangles;
    std::vector<TrackLine> wire_lines;
};

struct TrackTexture {
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool clamp_u = false;
    bool clamp_v = false;
    std::vector<std::uint8_t> rgba;
};

struct TrackBounds {
    Vec3 minimum{};
    Vec3 maximum{};
    bool valid = false;

    Vec3 center() const;
    float radius() const;
};

struct TrackViewModel {
    TrackMesh surface;
    TrackMesh visual_walls;
    TrackMesh collision_walls;
    TrackMesh scenery;
    std::vector<TrackTexture> textures;
    std::vector<Vec3> centerline;
    TrackBounds bounds;
    std::filesystem::path source_path;
    std::string source_description;
    std::vector<std::string> resource_search_order;
    std::uint32_t segment_count = 0;
    std::uint32_t material_count = 0;
    std::uint32_t wall_count = 0;
    std::uint32_t object_count = 0;
    std::uint32_t missing_object_count = 0;
    struct MissingObject {
        std::string name;
        std::string reason;
        std::uint32_t instance_count = 0;
    };
    std::vector<MissingObject> missing_objects;
};

struct TrackCamera {
    float yaw = -0.75f;
    float pitch = 0.82f;
    float zoom = 1.0f;
    Vec3 target{};
};

struct TrackFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;
};

TrackViewModel load_track_view(
    const std::filesystem::path& path,
    const std::optional<std::filesystem::path>& shared_folder = std::nullopt);

TrackFrame render_track_view(const TrackViewModel& model,
                             const TrackCamera& camera,
                             TrackViewModes modes,
                             int width,
                             int height);

void write_track_view_bmp(const std::filesystem::path& path,
                          const TrackFrame& frame);

}  // namespace opennr::viewer
