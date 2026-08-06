#include "viewer/track_view.h"

#include "fs/dat_archive.h"
#include "fs/papyrus_archive.h"
#include "fs/papyrus_descriptors.h"
#include "render/image_loader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace opennr::viewer {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct ProfileBoundary {
    float lateral_start = 0.0f;
    float lateral_end = 0.0f;
    double height_start = 0.0;
    double height_end = 0.0;
    float slope_start = 0.0f;
    float slope_end = 0.0f;
    std::uint8_t visual_curve_mode = 0;
};

struct MaterialBoundary {
    float lateral_start = 0.0f;
    float lateral_end = 0.0f;
    std::uint32_t surface_code = 0;
    std::uint8_t boundary_mode = 0;
    std::string texture_name;
    std::array<float, 4> u{};
    std::array<float, 4> v{};
    bool has_uv = false;
};

struct WallFace {
    std::string texture_name;
    std::array<float, 4> u{};
    std::array<float, 4> v{};
    bool has_uv = false;
};

struct WallRecord {
    float start = 0.0f;
    WallFace faces[3];
};

struct WallBoundary {
    float lateral_start = 0.0f;
    float lateral_end = 0.0f;
    float height_start = 0.0f;
    float height_end = 0.0f;
    float visual_width_start = 0.0f;
    float visual_width_end = 0.0f;
    float collision_width_start = 0.0f;
    float collision_width_end = 0.0f;
    bool height_along_profile_normal = false;
    std::uint32_t surface_code = 0;
    std::uint8_t boundary_mode = 0;
    std::vector<WallRecord> records;
};

struct TsoInstance {
    std::string name;
    double transform[6]{};
};

struct SegmentGeometry {
    int kind = 0;
    Vec3 start{};
    Vec3 end{};
    float heading_start = 0.0f;
    float heading_end = 0.0f;
    std::vector<ProfileBoundary> profile;
    std::vector<MaterialBoundary> materials;
    std::vector<WallBoundary> walls;
};

struct ProfilePoint {
    float lateral = 0.0f;
    float height = 0.0f;
};

struct ParsedTrack {
    std::vector<SegmentGeometry> segments;
    std::vector<TsoInstance> objects;
    std::uint32_t declared_segment_count = 0;
};

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string filename_from_archive_name(std::string_view value) {
    const auto split = value.find_last_of("/\\");
    return std::string(value.substr(split == std::string_view::npos ? 0 : split + 1));
}

bool has_extension(const std::filesystem::path& path, std::string_view extension) {
    return lower_ascii(path.extension().string()) == lower_ascii(std::string(extension));
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    const auto end = input.tellg();
    if (end < 0) throw std::runtime_error("cannot read the size of " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) throw std::runtime_error("cannot read " + path.string());
    return bytes;
}

struct PtfSource {
    std::vector<std::uint8_t> bytes;
    std::filesystem::path source_path;
    std::string description;
    struct ResourceStore;
    std::shared_ptr<ResourceStore> resources;
};

std::string resource_key(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.starts_with("./")) value.erase(0, 2);
    return lower_ascii(std::move(value));
}

struct PtfSource::ResourceStore {
    struct ArchiveResource {
        std::filesystem::path path;
        DatArchive archive;
    };

    struct Layer {
        std::filesystem::path folder;
        std::optional<ArchiveResource> archive;
        std::unordered_map<std::string, std::size_t> archive_entries;
        std::unordered_map<std::string, std::size_t> archive_base_entries;
        std::unordered_map<std::string, std::filesystem::path> loose_files;
    };

    std::vector<Layer> layers;

    void add_layer(const std::filesystem::path& folder,
                   const std::optional<std::filesystem::path>& archive_path) {
        Layer layer;
        layer.folder = folder;
        if (archive_path) {
            layer.archive.emplace(ArchiveResource{
                *archive_path, DatArchive::load(*archive_path)});
            const auto& entries = layer.archive->archive.entries();
            for (std::size_t i = 0; i < entries.size(); ++i) {
                layer.archive_entries.try_emplace(resource_key(entries[i].name), i);
                layer.archive_base_entries.try_emplace(
                    resource_key(filename_from_archive_name(entries[i].name)), i);
            }
        }
        std::vector<std::filesystem::path> loose_paths;
        for (const auto& item : std::filesystem::directory_iterator(folder)) {
            if (!item.is_regular_file()) continue;
            const auto extension = lower_ascii(item.path().extension().string());
            if (extension != ".3do" && extension != ".mip") continue;
            loose_paths.push_back(item.path());
        }
        std::stable_sort(loose_paths.begin(), loose_paths.end(), [](const auto& a,
                                                                    const auto& b) {
            return lower_ascii(a.filename().string()) <
                   lower_ascii(b.filename().string());
        });
        for (const auto& path : loose_paths) {
            layer.loose_files.try_emplace(
                resource_key(path.filename().string()), path);
        }
        layers.push_back(std::move(layer));
    }

    std::optional<std::vector<std::uint8_t>> read(std::string_view requested) const {
        const std::string key = resource_key(std::string(requested));
        const std::string base = resource_key(filename_from_archive_name(requested));
        for (const auto& layer : layers) {
            if (layer.archive) {
                const auto exact = layer.archive_entries.find(key);
                const auto fallback = layer.archive_base_entries.find(base);
                const auto selected = exact != layer.archive_entries.end()
                    ? exact->second
                    : fallback != layer.archive_base_entries.end()
                        ? fallback->second : std::numeric_limits<std::size_t>::max();
                if (selected != std::numeric_limits<std::size_t>::max()) {
                    return layer.archive->archive.read(
                        layer.archive->archive.entries()[selected]);
                }
            }
            const std::filesystem::path relative =
                std::filesystem::path(key).lexically_normal();
            bool safe_relative = !relative.empty() && !relative.is_absolute();
            for (const auto& part : relative) {
                if (part == "..") safe_relative = false;
            }
            if (safe_relative &&
                (has_extension(relative, ".3do") || has_extension(relative, ".mip"))) {
                const auto exact_loose = layer.folder / relative;
                if (std::filesystem::is_regular_file(exact_loose)) {
                    return read_file(exact_loose);
                }
            }
            const auto loose = layer.loose_files.find(base);
            if (loose != layer.loose_files.end()) {
                return read_file(loose->second);
            }
        }
        return std::nullopt;
    }
};

void prefer_named_paths(std::vector<std::filesystem::path>& paths,
                        const std::vector<std::string>& preferred_stems) {
    const auto rank = [&](const std::filesystem::path& path) {
        const std::string stem = lower_ascii(path.stem().string());
        const auto found = std::find(preferred_stems.begin(), preferred_stems.end(), stem);
        return found == preferred_stems.end()
            ? preferred_stems.size()
            : static_cast<std::size_t>(found - preferred_stems.begin());
    };
    std::stable_sort(paths.begin(), paths.end(), [&](const auto& a, const auto& b) {
        const auto ar = rank(a), br = rank(b);
        return ar != br ? ar < br : lower_ascii(a.string()) < lower_ascii(b.string());
    });
}

std::vector<std::filesystem::path> dat_files_in(
        const std::filesystem::path& folder,
        const std::vector<std::string>& preferred_stems) {
    std::vector<std::filesystem::path> archives;
    for (const auto& item : std::filesystem::directory_iterator(folder)) {
        if (item.is_regular_file() && has_extension(item.path(), ".dat")) {
            archives.push_back(item.path());
        }
    }
    prefer_named_paths(archives, preferred_stems);
    return archives;
}

std::shared_ptr<PtfSource::ResourceStore> make_resource_store(
        const std::filesystem::path& track_folder,
        const std::optional<std::filesystem::path>& track_archive,
        const std::optional<std::filesystem::path>& shared_folder) {
    auto resources = std::make_shared<PtfSource::ResourceStore>();
    resources->add_layer(track_folder, track_archive);
    if (shared_folder) {
        if (!std::filesystem::is_directory(*shared_folder)) {
            throw std::runtime_error("the shared resource path is not a folder");
        }
        const std::string folder_stem = lower_ascii(shared_folder->filename().string());
        const auto archives = dat_files_in(*shared_folder, {"shared", folder_stem});
        resources->add_layer(*shared_folder,
            archives.empty() ? std::nullopt
                             : std::optional<std::filesystem::path>(archives.front()));
    }
    return resources;
}

PtfSource find_ptf_in_folder(
        const std::filesystem::path& folder,
        const std::optional<std::filesystem::path>& shared_folder) {
    const std::string folder_name = lower_ascii(folder.filename().string());
    std::vector<std::filesystem::path> direct_ptfs;

    for (const auto& item : std::filesystem::directory_iterator(folder)) {
        if (!item.is_regular_file()) continue;
        if (has_extension(item.path(), ".ptf")) direct_ptfs.push_back(item.path());
    }
    prefer_named_paths(direct_ptfs, {folder_name});
    std::vector<std::string> preferred_archives;
    if (!direct_ptfs.empty()) {
        preferred_archives.push_back(lower_ascii(direct_ptfs.front().stem().string()));
    }
    if (preferred_archives.empty() || preferred_archives.front() != folder_name) {
        preferred_archives.push_back(folder_name);
    }
    auto archives = dat_files_in(folder, preferred_archives);
    if (!direct_ptfs.empty()) {
        const auto track_archive = archives.empty()
            ? std::nullopt
            : std::optional<std::filesystem::path>(archives.front());
        auto resources = make_resource_store(folder, track_archive, shared_folder);
        return {read_file(direct_ptfs.front()), direct_ptfs.front(),
                direct_ptfs.front().filename().string(), resources};
    }

    for (const auto& archive_path : archives) {
        auto archive = DatArchive::load(archive_path);
        std::vector<const DatEntry*> entries;
        for (const auto& entry : archive.entries()) {
            const auto name = filename_from_archive_name(entry.name);
            if (lower_ascii(std::filesystem::path(name).extension().string()) == ".ptf") {
                entries.push_back(&entry);
            }
        }
        std::stable_sort(entries.begin(), entries.end(), [&](const auto* a, const auto* b) {
            const bool am = lower_ascii(std::filesystem::path(
                filename_from_archive_name(a->name)).stem().string()) == folder_name;
            const bool bm = lower_ascii(std::filesystem::path(
                filename_from_archive_name(b->name)).stem().string()) == folder_name;
            return am != bm ? am : lower_ascii(a->name) < lower_ascii(b->name);
        });
        if (!entries.empty()) {
            auto resources = make_resource_store(folder, archive_path, shared_folder);
            return {archive.read(*entries.front()), archive_path,
                    archive_path.filename().string() + " : " + entries.front()->name,
                    resources};
        }
    }

    throw std::runtime_error("the track folder has no PTF file or DAT entry");
}

PtfSource load_ptf_source(
        const std::filesystem::path& path,
        const std::optional<std::filesystem::path>& shared_folder) {
    if (std::filesystem::is_directory(path)) {
        return find_ptf_in_folder(path, shared_folder);
    }
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("the selected path is not a file or folder");
    }
    if (!has_extension(path, ".ptf")) {
        throw std::runtime_error("select a track folder or a PTF file");
    }
    const auto folder = path.parent_path().empty()
        ? std::filesystem::current_path() : path.parent_path();
    const std::string file_stem = lower_ascii(path.stem().string());
    const std::string folder_stem = lower_ascii(folder.filename().string());
    const auto archives = dat_files_in(folder, {file_stem, folder_stem});
    const auto track_archive = archives.empty()
        ? std::nullopt
        : std::optional<std::filesystem::path>(archives.front());
    auto resources = make_resource_store(folder, track_archive, shared_folder);
    return {read_file(path), path, path.filename().string(), resources};
}

float normalized_turn(const SegmentGeometry& segment) {
    float turn = segment.heading_end - segment.heading_start;
    while (turn > kPi) turn -= 2.0f * kPi;
    while (turn < -kPi) turn += 2.0f * kPi;
    return turn;
}

float segment_length(const SegmentGeometry& segment) {
    const float dx = segment.end.x - segment.start.x;
    const float dy = segment.end.y - segment.start.y;
    const float chord = std::hypot(dx, dy);
    const float turn = std::fabs(normalized_turn(segment));
    if (segment.kind != 1 || turn < 1.0e-5f) return chord;
    const float divisor = 2.0f * std::sin(turn * 0.5f);
    return std::fabs(divisor) > 1.0e-6f ? chord * turn / divisor : chord;
}

void sample_segment(const SegmentGeometry& segment, float t,
                    Vec3& position, float& heading) {
    const float turn = normalized_turn(segment);
    heading = segment.heading_start + turn * t;
    if (segment.kind != 1 || std::fabs(turn) < 1.0e-5f) {
        position = segment.start + (segment.end - segment.start) * t;
        return;
    }
    const float chord = std::hypot(segment.end.x - segment.start.x,
                                   segment.end.y - segment.start.y);
    const float signed_radius = chord /
        (2.0f * std::sin(std::fabs(turn) * 0.5f)) * (turn >= 0.0f ? 1.0f : -1.0f);
    position = {
        segment.start.x + signed_radius *
            (std::sin(heading) - std::sin(segment.heading_start)),
        segment.start.y - signed_radius *
            (std::cos(heading) - std::cos(segment.heading_start)),
        0.0f,
    };
}

Vec3 plan_boundary_point(const SegmentGeometry& segment, float t,
                         float lateral_start, float lateral_end,
                         std::uint8_t boundary_mode) {
    const Vec3 left_start{-std::sin(segment.heading_start),
                          std::cos(segment.heading_start), 0.0f};
    const Vec3 left_end{-std::sin(segment.heading_end),
                        std::cos(segment.heading_end), 0.0f};
    const Vec3 start = segment.start + left_start * lateral_start;
    const Vec3 end = segment.end + left_end * lateral_end;
    if (segment.kind == 0 || boundary_mode != 0) {
        return start + (end - start) * t;
    }
    Vec3 center;
    float heading = 0.0f;
    sample_segment(segment, t, center, heading);
    const float lateral = lateral_start + (lateral_end - lateral_start) * t;
    const Vec3 left{-std::sin(heading), std::cos(heading), 0.0f};
    return center + left * lateral;
}

float boundary_lateral_at(const SegmentGeometry& segment, float t,
                          float lateral_start, float lateral_end,
                          std::uint8_t boundary_mode) {
    Vec3 center;
    float heading = 0.0f;
    sample_segment(segment, t, center, heading);
    const Vec3 left{-std::sin(heading), std::cos(heading), 0.0f};
    return dot(plan_boundary_point(segment, t, lateral_start, lateral_end,
                                   boundary_mode) - center, left);
}

float material_boundary_value(const SegmentGeometry& segment,
                              const MaterialBoundary& boundary,
                              float t, float lateral) {
    if (boundary.boundary_mode == 0) {
        return boundary.lateral_start +
            t * (boundary.lateral_end - boundary.lateral_start) - lateral;
    }
    Vec3 center;
    float heading = 0.0f;
    sample_segment(segment, t, center, heading);
    const Vec3 left{-std::sin(heading), std::cos(heading), 0.0f};
    const Vec3 point = center + left * lateral;
    const Vec3 middle = plan_boundary_point(
        segment, 0.5f, boundary.lateral_start, boundary.lateral_end,
        boundary.boundary_mode);
    const Vec3 end = plan_boundary_point(
        segment, 1.0f, boundary.lateral_start, boundary.lateral_end,
        boundary.boundary_mode);
    const Vec3 line = end - middle;
    return (point.x - middle.x) * line.y -
           (point.y - middle.y) * line.x;
}

int segment_subdivisions(const SegmentGeometry& segment) {
    // Four-metre edges keep a 200 m radius corner within about one centimetre
    // of its authored arc. The angular limit also protects short, tight arcs.
    const float length = segment_length(segment);
    const float turn = std::fabs(normalized_turn(segment));
    float lateral_motion = 0.0f;
    const auto include_motion = [&](float start, float end) {
        lateral_motion = std::max(lateral_motion, std::fabs(end - start));
    };
    for (const auto& boundary : segment.profile) {
        include_motion(boundary.lateral_start, boundary.lateral_end);
    }
    for (const auto& boundary : segment.materials) {
        include_motion(boundary.lateral_start, boundary.lateral_end);
    }
    for (const auto& wall : segment.walls) {
        include_motion(wall.lateral_start, wall.lateral_end);
        include_motion(wall.visual_width_start, wall.visual_width_end);
        include_motion(wall.collision_width_start, wall.collision_width_end);
    }
    const int by_length = static_cast<int>(std::ceil(length / 4.0f));
    const int by_angle = segment.kind == 1
        ? static_cast<int>(std::ceil(turn / 0.02f)) : 1;
    const int by_lateral_motion = static_cast<int>(
        std::ceil(lateral_motion / 0.75f));
    return std::clamp(
        std::max({1, by_length, by_angle, by_lateral_motion}), 1, 1024);
}

float boundary_height(const SegmentGeometry& segment,
                      const ProfileBoundary& boundary, float t) {
    const double chord = std::hypot(
        static_cast<double>(segment.end.x - segment.start.x),
        static_cast<double>(segment.end.y - segment.start.y));
    double length_start = chord;
    double length_end = chord;
    const double turn = std::fabs(static_cast<double>(normalized_turn(segment)));
    if (segment.kind == 1 && boundary.visual_curve_mode == 0 && turn > 1.0e-8) {
        const Vec3 left_start{-std::sin(segment.heading_start),
                              std::cos(segment.heading_start), 0.0f};
        const Vec3 left_end{-std::sin(segment.heading_end),
                            std::cos(segment.heading_end), 0.0f};
        const double scale = turn / (2.0 * std::sin(turn * 0.5));
        const auto offset_length = [&](float lateral) {
            const Vec3 start = segment.start + left_start * lateral;
            const Vec3 end = segment.end + left_end * lateral;
            return static_cast<double>((end - start).length()) * scale;
        };
        length_start = offset_length(boundary.lateral_start);
        length_end = offset_length(boundary.lateral_end);
    }
    const double z0 = boundary.height_start;
    const double z1 = boundary.height_end;
    const double m0 = length_start * std::tan(boundary.slope_start);
    const double m1 = length_end * std::tan(boundary.slope_end);
    const double c3 = 2.0 * z0 - 2.0 * z1 + m0 + m1;
    const double c2 = -3.0 * z0 + 3.0 * z1 - 2.0 * m0 - m1;
    return static_cast<float>(((c3 * t + c2) * t + m0) * t + z0);
}

std::vector<ProfilePoint> profile_at(const SegmentGeometry& segment, float t) {
    std::vector<ProfilePoint> profile;
    profile.reserve(segment.profile.size());
    for (const auto& boundary : segment.profile) {
        profile.push_back({
            boundary.lateral_start +
                (boundary.lateral_end - boundary.lateral_start) * t,
            boundary_height(segment, boundary, t),
        });
    }
    if (profile.size() < 2) profile = {{-8.0f, 0.0f}, {8.0f, 0.0f}};
    std::stable_sort(profile.begin(), profile.end(), [](const auto& a, const auto& b) {
        return a.lateral < b.lateral;
    });
    return profile;
}

float height_at(const std::vector<ProfilePoint>& profile, float lateral) {
    if (profile.empty()) return 0.0f;
    if (lateral <= profile.front().lateral) return profile.front().height;
    if (lateral >= profile.back().lateral) return profile.back().height;
    for (std::size_t i = 0; i + 1 < profile.size(); ++i) {
        const auto& a = profile[i];
        const auto& b = profile[i + 1];
        if (lateral < a.lateral || lateral > b.lateral) continue;
        const float span = b.lateral - a.lateral;
        const float t = std::fabs(span) > 1.0e-6f ? (lateral - a.lateral) / span : 0.0f;
        return a.height + (b.height - a.height) * t;
    }
    return profile.back().height;
}

std::vector<ProfilePoint> shared_join_profile(
        const std::vector<ProfilePoint>& previous_end,
        const std::vector<ProfilePoint>& next_start) {
    if (previous_end.size() < 2) return next_start;
    if (next_start.size() < 2) return previous_end;

    const float lower = std::min(previous_end.front().lateral,
                                 next_start.front().lateral);
    const float upper = std::max(previous_end.back().lateral,
                                 next_start.back().lateral);
    if (upper <= lower) return previous_end;

    std::vector<float> laterals{lower, upper};
    const auto append = [&](const std::vector<ProfilePoint>& profile) {
        for (const auto& point : profile) {
            if (point.lateral >= lower && point.lateral <= upper) {
                laterals.push_back(point.lateral);
            }
        }
    };
    append(previous_end);
    append(next_start);
    std::sort(laterals.begin(), laterals.end());
    laterals.erase(std::unique(laterals.begin(), laterals.end(),
        [](float a, float b) { return std::fabs(a - b) < 1.0e-4f; }),
        laterals.end());

    std::vector<ProfilePoint> shared;
    shared.reserve(laterals.size());
    for (float lateral : laterals) {
        // Both descriptors describe the same station. Averaging removes
        // round-off and gives inserted breakpoints one shared position.
        shared.push_back({lateral,
            0.5f * (height_at(previous_end, lateral) +
                    height_at(next_start, lateral))});
    }
    return shared;
}

std::uint32_t surface_color(std::uint32_t code) {
    switch (code) {
        case 0x001: return 0x4c4e52;
        case 0x002:
        case 0x009:
        case 0x801:
        case 0x809: return 0x8a8983;
        case 0x00c: return 0xb43b35;
        case 0x003: return 0xd2bd4c;
        case 0x004: return 0x3f753d;
        case 0x005: return 0x785438;
        case 0x006: return 0xbda76c;
        case 0x007: return 0x77756f;
        case 0x008: return 0x356b91;
        case 0x802:
        case 0x80b: return 0xa8aaad;
        case 0x803: return 0x343436;
        case 0x804: return 0x9a793d;
        case 0x805:
        case 0x80a: return 0x777d80;
        case 0x806: return 0x4c703c;
        case 0x807: return 0x2d5734;
        case 0x808: return 0x795f43;
        default: {
            const std::uint32_t hash = code * 2654435761u;
            return 0x555555u | ((hash >> 8) & 0x3f3f3fu);
        }
    }
}

void include_point(TrackBounds& bounds, Vec3 point) {
    if (!bounds.valid) {
        bounds.minimum = point;
        bounds.maximum = point;
        bounds.valid = true;
        return;
    }
    bounds.minimum.x = std::min(bounds.minimum.x, point.x);
    bounds.minimum.y = std::min(bounds.minimum.y, point.y);
    bounds.minimum.z = std::min(bounds.minimum.z, point.z);
    bounds.maximum.x = std::max(bounds.maximum.x, point.x);
    bounds.maximum.y = std::max(bounds.maximum.y, point.y);
    bounds.maximum.z = std::max(bounds.maximum.z, point.z);
}

std::uint32_t add_vertex(TrackMesh& mesh, TrackBounds& bounds, Vec3 point,
                         float u = 0.0f, float v = 0.0f) {
    include_point(bounds, point);
    mesh.vertices.push_back({point, u, v});
    return static_cast<std::uint32_t>(mesh.vertices.size() - 1);
}

void add_triangle(TrackMesh& mesh, std::uint32_t a, std::uint32_t b,
                  std::uint32_t c, std::uint32_t color, std::uint32_t code,
                  std::int32_t texture = -1) {
    mesh.triangles.push_back({a, b, c, color, code, texture});
}

void add_quad(TrackMesh& mesh, TrackBounds& bounds,
              Vec3 a, Vec3 b, Vec3 c, Vec3 d,
              std::uint32_t color, std::uint32_t code,
              std::int32_t texture = -1,
              std::array<float, 2> uv_a = {},
              std::array<float, 2> uv_b = {},
              std::array<float, 2> uv_c = {},
              std::array<float, 2> uv_d = {}) {
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    add_vertex(mesh, bounds, a, uv_a[0], uv_a[1]);
    add_vertex(mesh, bounds, b, uv_b[0], uv_b[1]);
    add_vertex(mesh, bounds, c, uv_c[0], uv_c[1]);
    add_vertex(mesh, bounds, d, uv_d[0], uv_d[1]);
    add_triangle(mesh, base, base + 1, base + 2, color, code, texture);
    add_triangle(mesh, base + 2, base + 1, base + 3, color, code, texture);
    mesh.wire_lines.push_back({base, base + 1});
    mesh.wire_lines.push_back({base + 1, base + 3});
    mesh.wire_lines.push_back({base + 3, base + 2});
    mesh.wire_lines.push_back({base + 2, base});
}

float mapped_coordinate(const std::array<float, 4>& channel,
                        float t, float lateral_fraction) {
    const float start = channel[0] + (channel[3] - channel[0]) * lateral_fraction;
    const float end = channel[1] + (channel[2] - channel[1]) * lateral_fraction;
    return start + (end - start) * t;
}

template <class Mapping>
std::array<float, 2> mapped_uv(const Mapping& mapping,
                               float t, float lateral_fraction) {
    if (!mapping.has_uv) return {lateral_fraction, t};
    return {mapped_coordinate(mapping.u, t, lateral_fraction),
            mapped_coordinate(mapping.v, t, lateral_fraction)};
}

class ModelResourceLoader {
public:
    ModelResourceLoader(TrackViewModel& model,
                        const std::shared_ptr<PtfSource::ResourceStore>& resources)
        : model_(model), resources_(resources) {}

    std::int32_t texture(std::string_view name) {
        if (name.empty() || !resources_) return -1;
        const std::string key = resource_key(std::string(name));
        if (const auto found = texture_cache_.find(key);
            found != texture_cache_.end()) return found->second;
        texture_cache_[key] = -1;
        try {
            const auto bytes = resources_->read(name);
            if (!bytes) return -1;
            const auto image = render::decode_mip_rgba8(*bytes);
            if (image.width == 0 || image.height == 0 || image.rgba8.empty()) return -1;
            TrackTexture texture;
            texture.name = std::string(name);
            texture.width = image.width;
            texture.height = image.height;
            texture.clamp_u = image.address_u == render::AddressMode::clamp;
            texture.clamp_v = image.address_v == render::AddressMode::clamp;
            texture.rgba = image.rgba8;
            const auto index = static_cast<std::int32_t>(model_.textures.size());
            model_.textures.push_back(std::move(texture));
            texture_cache_[key] = index;
            return index;
        } catch (const std::exception&) {
            return -1;
        }
    }

private:
    TrackViewModel& model_;
    std::shared_ptr<PtfSource::ResourceStore> resources_;
    std::unordered_map<std::string, std::int32_t> texture_cache_;
};

Vec3 point_at(const SegmentGeometry& segment, float t, float lateral,
              const std::vector<ProfilePoint>& profile) {
    Vec3 center;
    float heading = 0.0f;
    sample_segment(segment, t, center, heading);
    const Vec3 left{-std::sin(heading), std::cos(heading), 0.0f};
    return center + left * lateral + Vec3{0.0f, 0.0f, height_at(profile, lateral)};
}

void build_surface_step(TrackViewModel& model, const SegmentGeometry& segment,
                        float t0, float t1, ModelResourceLoader& resources,
                        const std::vector<ProfilePoint>* profile0 = nullptr,
                        const std::vector<ProfilePoint>* profile1 = nullptr) {
    const auto p0 = profile0 ? *profile0 : profile_at(segment, t0);
    const auto p1 = profile1 ? *profile1 : profile_at(segment, t1);
    const float lo0 = p0.front().lateral;
    const float hi0 = p0.back().lateral;
    const float lo1 = p1.front().lateral;
    const float hi1 = p1.back().lateral;
    if (hi0 <= lo0 || hi1 <= lo1) return;

    struct BoundaryTrack {
        float at_start = 0.0f;
        float at_end = 0.0f;
    };
    std::vector<BoundaryTrack> boundaries{{lo0, lo1}, {hi0, hi1}};
    const auto add_fraction_track = [&](float fraction) {
        if (!std::isfinite(fraction) || fraction <= 0.0f || fraction >= 1.0f) return;
        boundaries.push_back({
            lo0 + (hi0 - lo0) * fraction,
            lo1 + (hi1 - lo1) * fraction,
        });
    };
    for (const auto& point : p0) {
        add_fraction_track((point.lateral - lo0) / (hi0 - lo0));
    }
    for (const auto& point : p1) {
        add_fraction_track((point.lateral - lo1) / (hi1 - lo1));
    }
    for (const auto& material : segment.materials) {
        const float lateral0 = boundary_lateral_at(
            segment, t0, material.lateral_start, material.lateral_end,
            material.boundary_mode);
        const float lateral1 = boundary_lateral_at(
            segment, t1, material.lateral_start, material.lateral_end,
            material.boundary_mode);
        if (lateral0 > lo0 && lateral0 < hi0 &&
            lateral1 > lo1 && lateral1 < hi1) {
            boundaries.push_back({lateral0, lateral1});
        }
    }
    std::stable_sort(boundaries.begin(), boundaries.end(),
        [](const auto& a, const auto& b) {
            return a.at_start + a.at_end < b.at_start + b.at_end;
        });
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end(),
        [](const auto& a, const auto& b) {
            return std::fabs(a.at_start - b.at_start) < 1.0e-5f &&
                   std::fabs(a.at_end - b.at_end) < 1.0e-5f;
        }), boundaries.end());

    const auto material_at = [&](float t, float lateral) -> std::size_t {
        if (segment.materials.empty()) return segment.materials.size();
        std::size_t selected = 0;
        for (std::size_t i = 0; i < segment.materials.size(); ++i) {
            if (material_boundary_value(segment, segment.materials[i],
                                        t, lateral) < 0.0f) {
                selected = i;
            } else {
                break;
            }
        }
        return selected;
    };

    for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
        const float a0 = boundaries[i].at_start;
        const float a1 = boundaries[i + 1].at_start;
        const float b0 = boundaries[i].at_end;
        const float b1 = boundaries[i + 1].at_end;
        const float mid_t = (t0 + t1) * 0.5f;
        const float mid_lateral = (a0 + a1 + b0 + b1) * 0.25f;
        const std::size_t material_index = material_at(mid_t, mid_lateral);
        const MaterialBoundary* material = material_index < segment.materials.size()
            ? &segment.materials[material_index] : nullptr;
        const std::uint32_t code = material ? material->surface_code : 0x001u;
        const std::int32_t texture = material
            ? resources.texture(material->texture_name) : -1;
        auto material_fraction = [&](float t, float lateral) {
            if (!material || material_index + 1 >= segment.materials.size()) return 0.0f;
            const auto& next = segment.materials[material_index + 1];
            const float lo = boundary_lateral_at(segment, t,
                material->lateral_start, material->lateral_end,
                material->boundary_mode);
            const float hi = boundary_lateral_at(segment, t,
                next.lateral_start, next.lateral_end, next.boundary_mode);
            return std::fabs(hi - lo) > 1.0e-6f ? (lateral - lo) / (hi - lo) : 0.0f;
        };
        add_quad(model.surface, model.bounds,
                 point_at(segment, t0, a0, p0),
                 point_at(segment, t1, b0, p1),
                 point_at(segment, t0, a1, p0),
                 point_at(segment, t1, b1, p1), surface_color(code), code,
                 texture,
                 material ? mapped_uv(*material, t0, material_fraction(t0, a0))
                          : std::array<float, 2>{},
                 material ? mapped_uv(*material, t1, material_fraction(t1, b0))
                          : std::array<float, 2>{},
                 material ? mapped_uv(*material, t0, material_fraction(t0, a1))
                          : std::array<float, 2>{},
                 material ? mapped_uv(*material, t1, material_fraction(t1, b1))
                          : std::array<float, 2>{});
    }
}

Vec3 wall_top_offset(float lateral, float height, bool profile_normal,
                     const std::vector<ProfilePoint>& profile,
                     const Vec3& left) {
    if (profile_normal && profile.size() >= 2) {
        std::size_t upper = 1;
        while (upper + 1 < profile.size() && lateral > profile[upper].lateral) ++upper;
        const auto& a = profile[upper - 1];
        const auto& b = profile[upper];
        const float span = b.lateral - a.lateral;
        const float slope = std::fabs(span) > 1.0e-6f
            ? (b.height - a.height) / span : 0.0f;
        const float inv = 1.0f / std::sqrt(1.0f + slope * slope);
        return left * (-slope * height * inv) +
               Vec3{0.0f, 0.0f, height * inv};
    }
    return {0.0f, 0.0f, height};
}

void add_wall_step(TrackMesh& mesh, TrackBounds& bounds,
                   const SegmentGeometry& segment, const WallBoundary& wall,
                   float t0, float t1, bool collision,
                   ModelResourceLoader& resources,
                   const std::vector<ProfilePoint>* profile0 = nullptr,
                   const std::vector<ProfilePoint>* profile1 = nullptr) {
    const auto p0 = profile0 ? *profile0 : profile_at(segment, t0);
    const auto p1 = profile1 ? *profile1 : profile_at(segment, t1);
    const float lat[2] = {
        boundary_lateral_at(segment, t0, wall.lateral_start, wall.lateral_end,
                            wall.boundary_mode),
        boundary_lateral_at(segment, t1, wall.lateral_start, wall.lateral_end,
                            wall.boundary_mode),
    };
    const float height[2] = {
        wall.height_start + (wall.height_end - wall.height_start) * t0,
        wall.height_start + (wall.height_end - wall.height_start) * t1,
    };
    const float authored_width[2] = {
        std::fabs(collision ? wall.collision_width_start : wall.visual_width_start),
        std::fabs(collision ? wall.collision_width_end : wall.visual_width_end),
    };

    Vec3 center[2];
    Vec3 left[2];
    for (int end = 0; end < 2; ++end) {
        float heading = 0.0f;
        sample_segment(segment, end == 0 ? t0 : t1, center[end], heading);
        left[end] = {-std::sin(heading), std::cos(heading), 0.0f};
    }

    const float times[2] = {t0, t1};
    const std::vector<ProfilePoint>* profiles[2] = {&p0, &p1};
    Vec3 minus_bottom[2], plus_bottom[2], minus_top[2], plus_top[2];
    for (int end = 0; end < 2; ++end) {
        const float t = times[end];
        const auto& profile = *profiles[end];
        const float minus_lateral = boundary_lateral_at(
            segment, t, wall.lateral_start - authored_width[0],
            wall.lateral_end - authored_width[1], wall.boundary_mode);
        const float plus_lateral = boundary_lateral_at(
            segment, t, wall.lateral_start + authored_width[0],
            wall.lateral_end + authored_width[1], wall.boundary_mode);
        minus_bottom[end] = plan_boundary_point(
            segment, t, wall.lateral_start - authored_width[0],
            wall.lateral_end - authored_width[1], wall.boundary_mode);
        plus_bottom[end] = plan_boundary_point(
            segment, t, wall.lateral_start + authored_width[0],
            wall.lateral_end + authored_width[1], wall.boundary_mode);
        minus_bottom[end].z = height_at(profile, minus_lateral);
        plus_bottom[end].z = height_at(profile, plus_lateral);

        // Both meshes use the authored cross-section-normal mode. This keeps
        // the collision face aligned with the rendered wall on banked track.
        const Vec3 top_offset = wall_top_offset(
            lat[end], height[end],
            wall.height_along_profile_normal, profile, left[end]);
        minus_top[end] = minus_bottom[end] + top_offset;
        plus_top[end] = plus_bottom[end] + top_offset;
    }
    const std::uint32_t color = collision ? 0xd9574f : surface_color(wall.surface_code);

    const WallRecord* record = nullptr;
    float range_start = 0.0f;
    float range_end = 1.0f;
    if (!collision && !wall.records.empty()) {
        const float midpoint = (t0 + t1) * 0.5f;
        std::size_t selected = 0;
        for (std::size_t i = 1; i < wall.records.size(); ++i) {
            if (wall.records[i].start > midpoint) break;
            selected = i;
        }
        record = &wall.records[selected];
        range_start = record->start;
        if (selected + 1 < wall.records.size()) range_end = wall.records[selected + 1].start;
    }
    const float range_span = std::max(1.0e-6f, range_end - range_start);
    const float q0 = std::clamp((t0 - range_start) / range_span, 0.0f, 1.0f);
    const float q1 = std::clamp((t1 - range_start) / range_span, 0.0f, 1.0f);
    const auto face_texture = [&](int face) {
        return record ? resources.texture(record->faces[face].texture_name) : -1;
    };
    const auto face_uv = [&](int face, float q, float across) {
        return record ? mapped_uv(record->faces[face], q, across)
                      : std::array<float, 2>{across, q};
    };

    add_quad(mesh, bounds, minus_bottom[0], minus_bottom[1], minus_top[0], minus_top[1],
             color, wall.surface_code, collision ? -1 : face_texture(0),
             face_uv(0, q0, 0.0f), face_uv(0, q1, 0.0f),
             face_uv(0, q0, 1.0f), face_uv(0, q1, 1.0f));
    add_quad(mesh, bounds, plus_bottom[1], plus_bottom[0], plus_top[1], plus_top[0],
             color, wall.surface_code, collision ? -1 : face_texture(2),
             face_uv(2, q1, 1.0f), face_uv(2, q0, 1.0f),
             face_uv(2, q1, 0.0f), face_uv(2, q0, 0.0f));
    add_quad(mesh, bounds, minus_top[0], minus_top[1], plus_top[0], plus_top[1],
             color, wall.surface_code, collision ? -1 : face_texture(1),
             face_uv(1, q0, 0.0f), face_uv(1, q1, 0.0f),
             face_uv(1, q0, 1.0f), face_uv(1, q1, 1.0f));
}

template <class Mapping>
void copy_texture_mapping(Mapping& output,
                          const papyrus::PersistentObject* appearance_object,
                          const papyrus::PersistentObject* coordinates_object) {
    const auto* appearance = dynamic_cast<const papyrus::AppearanceDescriptor*>(
        appearance_object);
    const auto* texture = appearance
        ? dynamic_cast<const papyrus::TextureDescriptor*>(
              appearance->texture_slots[0].get())
        : nullptr;
    if (texture) output.texture_name = texture->texture_name;
    const auto* coordinates = dynamic_cast<const papyrus::TextureCoordsDescriptor*>(
        coordinates_object);
    if (!coordinates || coordinates->channels.size() < 2 ||
        coordinates->channels[0].size() < 4 ||
        coordinates->channels[1].size() < 4) return;
    for (std::size_t i = 0; i < 4; ++i) {
        output.u[i] = static_cast<float>(coordinates->channels[0][i]);
        output.v[i] = static_cast<float>(coordinates->channels[1][i]);
    }
    output.has_uv = true;
}

ParsedTrack parse_track(std::span<const std::uint8_t> bytes) {
    papyrus::register_all_descriptors();
    papyrus::Archive archive(bytes);
    auto root = std::dynamic_pointer_cast<papyrus::TrackDescriptor>(archive.read_object());
    if (!root) throw std::runtime_error("the PTF root is not a TrackDescriptor");
    ParsedTrack parsed;
    parsed.declared_segment_count =
        static_cast<std::uint32_t>(std::max(0, root->num_segments));

    if (archive.remaining() != 0) {
        const std::size_t start = archive.position();
        const bool padding = std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                         bytes.end(), [](std::uint8_t value) {
            return value == 0 || value == 0x20;
        });
        if (!padding) throw std::runtime_error("the PTF has non-padding trailing bytes");
    }

    parsed.segments.reserve(root->segments.size());
    for (const auto& object : root->segments) {
        const auto* source = dynamic_cast<const papyrus::SegmentDescriptor*>(object.get());
        if (!source || source->segment_kind == -1) continue;
        SegmentGeometry segment;
        segment.kind = source->segment_kind;
        segment.start = {static_cast<float>(source->pos_a),
                         static_cast<float>(source->pos_b), 0.0f};
        segment.end = {static_cast<float>(source->pos_d),
                       static_cast<float>(source->pos_e), 0.0f};
        segment.heading_start = static_cast<float>(source->angle_c);
        segment.heading_end = static_cast<float>(source->angle_f);

        for (const auto& item : source->x_sections) {
            const auto* x = dynamic_cast<const papyrus::X_SectionDescriptor*>(item.get());
            if (!x) continue;
            segment.profile.push_back({x->lateral_start, x->lateral_end,
                                       x->height_start, x->height_end,
                                       x->slope_start, x->slope_end,
                                       x->visual_curve_mode});
        }
        for (const auto& item : source->f_sections) {
            const auto* f = dynamic_cast<const papyrus::F_SectionDescriptor*>(item.get());
            if (!f) continue;
            MaterialBoundary material;
            material.lateral_start = f->lateral_start;
            material.lateral_end = f->lateral_end;
            material.surface_code = f->surface_code;
            material.boundary_mode = f->boundary_mode;
            copy_texture_mapping(material, f->x_section.get(), f->w_section.get());
            segment.materials.push_back(std::move(material));
        }
        for (const auto& item : source->w_sections) {
            const auto* w = dynamic_cast<const papyrus::W_SectionDescriptor*>(item.get());
            if (!w) continue;
            WallBoundary wall{
                w->lateral_start, w->lateral_end,
                static_cast<float>(w->height_start),
                static_cast<float>(w->height_end),
                static_cast<float>(w->visual_face_offset_start),
                static_cast<float>(w->visual_face_offset_end),
                static_cast<float>(w->collision_half_thickness_start),
                static_cast<float>(w->collision_half_thickness_end),
                w->height_offset_mode != 0, w->surface_code,
            };
            wall.boundary_mode = w->boundary_mode;
            wall.records.reserve(w->records.size());
            for (const auto& source_record : w->records) {
                WallRecord record;
                record.start = static_cast<float>(std::clamp(
                    source_record.longitudinal_position, 0.0, 1.0));
                for (int face = 0; face < 3; ++face) {
                    copy_texture_mapping(record.faces[face],
                        source_record.inner[face].child_a.get(),
                        source_record.inner[face].child_b.get());
                }
                wall.records.push_back(std::move(record));
            }
            std::stable_sort(wall.records.begin(), wall.records.end(),
                [](const auto& a, const auto& b) { return a.start < b.start; });
            segment.walls.push_back(std::move(wall));
        }
        parsed.segments.push_back(std::move(segment));
    }
    for (const auto* list : {&root->children_b, &root->children_c}) {
        for (const auto& object : *list) {
            const auto* reference =
                dynamic_cast<const papyrus::TSOReferenceDescriptor*>(object.get());
            if (!reference) continue;
            TsoInstance instance;
            instance.name = reference->name;
            if (instance.name.empty()) {
                const auto* descriptor = dynamic_cast<const papyrus::TSODescriptor*>(
                    reference->child.get());
                if (descriptor) instance.name = descriptor->name_a;
            }
            if (instance.name.empty() ||
                !has_extension(std::filesystem::path(instance.name), ".3do")) continue;
            std::copy(std::begin(reference->xform), std::end(reference->xform),
                      std::begin(instance.transform));
            parsed.objects.push_back(std::move(instance));
        }
    }
    if (parsed.segments.empty()) {
        throw std::runtime_error("the PTF has no usable track segments");
    }
    return parsed;
}

struct Matrix4 {
    std::array<double, 16> value{};

    static Matrix4 identity() {
        Matrix4 matrix;
        matrix.value[0] = matrix.value[5] = matrix.value[10] = matrix.value[15] = 1.0;
        return matrix;
    }
};

Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 result;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 4; ++k) {
                result.value[column * 4 + row] +=
                    a.value[k * 4 + row] * b.value[column * 4 + k];
            }
        }
    }
    return result;
}

Matrix4 make_transform(double x, double y, double z,
                       double yaw, double pitch, double roll) {
    const double cy = std::cos(yaw), sy = std::sin(yaw);
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double cr = std::cos(roll), sr = std::sin(roll);
    Matrix4 matrix;
    matrix.value = {
        cy * cp, sy * cp, -sp, 0.0,
        cy * sp * sr - sy * cr, sy * sp * sr + cy * cr, cp * sr, 0.0,
        cy * sp * cr + sy * sr, sy * sp * cr - cy * sr, cp * cr, 0.0,
        x, y, z, 1.0,
    };
    return matrix;
}

Vec3 transform_point(const Matrix4& matrix, Vec3 point) {
    return {
        static_cast<float>(matrix.value[0] * point.x + matrix.value[4] * point.y +
                           matrix.value[8] * point.z + matrix.value[12]),
        static_cast<float>(matrix.value[1] * point.x + matrix.value[5] * point.y +
                           matrix.value[9] * point.z + matrix.value[13]),
        static_cast<float>(matrix.value[2] * point.x + matrix.value[6] * point.y +
                           matrix.value[10] * point.z + matrix.value[14]),
    };
}

const papyrus::PersistentObject* next_primitive(
        const papyrus::PersistentObject* primitive) {
    if (const auto* list = dynamic_cast<const papyrus::TriListDescriptor*>(primitive)) {
        return list->next_primitive.get();
    }
    if (const auto* strip = dynamic_cast<const papyrus::TriStripDescriptor*>(primitive)) {
        return strip->next_primitive.get();
    }
    if (const auto* fan = dynamic_cast<const papyrus::TriFanDescriptor*>(primitive)) {
        return fan->next_primitive.get();
    }
    return nullptr;
}

std::vector<std::uint32_t> triangle_indices(
        const papyrus::PersistentObject* primitive) {
    if (const auto* list = dynamic_cast<const papyrus::TriListDescriptor*>(primitive)) {
        return list->indices;
    }
    std::vector<std::uint32_t> output;
    if (const auto* strip = dynamic_cast<const papyrus::TriStripDescriptor*>(primitive)) {
        for (std::size_t i = 2; i < strip->indices.size(); ++i) {
            if ((i & 1u) == 0) {
                output.insert(output.end(), {strip->indices[i - 2], strip->indices[i - 1],
                                             strip->indices[i]});
            } else {
                output.insert(output.end(), {strip->indices[i - 1], strip->indices[i - 2],
                                             strip->indices[i]});
            }
        }
    } else if (const auto* fan = dynamic_cast<const papyrus::TriFanDescriptor*>(primitive)) {
        for (std::size_t i = 2; i < fan->indices.size(); ++i) {
            output.insert(output.end(), {fan->indices[0], fan->indices[i - 1],
                                         fan->indices[i]});
        }
    }
    return output;
}

TrackMesh load_scene_asset(std::span<const std::uint8_t> bytes,
                           ModelResourceLoader& resources) {
    papyrus::register_all_descriptors();
    papyrus::Archive archive(bytes);
    const auto root = archive.read_object();
    TrackMesh mesh;
    Matrix4 transform = Matrix4::identity();

    std::function<void(const papyrus::PersistentObject*, int)> visit;
    visit = [&](const papyrus::PersistentObject* object, int depth) {
        if (!object || depth > 128) return;
        const auto visit_first_renderable = [&](const auto& children) {
            for (const auto& child : children) {
                const auto vertex_count = mesh.vertices.size();
                const auto triangle_count = mesh.triangles.size();
                const Matrix4 saved = transform;
                visit(child.get(), depth + 1);
                transform = saved;
                if (mesh.triangles.size() > triangle_count) return;
                mesh.vertices.resize(vertex_count);
                mesh.triangles.resize(triangle_count);
            }
        };
        if (const auto* group = dynamic_cast<const papyrus::GroupDescriptor*>(object)) {
            for (const auto& child : group->children) visit(child.get(), depth + 1);
            return;
        }
        if (const auto* group = dynamic_cast<const papyrus::GroupingNodeDescriptor*>(object)) {
            visit_first_renderable(group->children);
            return;
        }
        if (const auto* lod = dynamic_cast<const papyrus::LodSwitchDescriptor*>(object)) {
            visit_first_renderable(lod->children);
            return;
        }
        if (const auto* state = dynamic_cast<const papyrus::StateSwitchDescriptor*>(object)) {
            visit_first_renderable(state->children);
            return;
        }
        if (const auto* node = dynamic_cast<const papyrus::TransformDescriptor*>(object)) {
            const Matrix4 saved = transform;
            transform = multiply(transform, make_transform(
                node->tx, node->ty, node->tz, node->yaw, node->pitch, node->roll));
            visit(node->child.get(), depth + 1);
            transform = saved;
            return;
        }
        if (const auto* node =
                dynamic_cast<const papyrus::AnimatedTransformDescriptor*>(object)) {
            const Matrix4 saved = transform;
            transform = multiply(transform, make_transform(
                node->tx, node->ty, node->tz, 0.0, 0.0, 0.0));
            visit(node->child.get(), depth + 1);
            transform = saved;
            return;
        }
        if (const auto* node = dynamic_cast<const papyrus::BillboardDescriptor*>(object)) {
            visit(node->child.get(), depth + 1);
            return;
        }
        if (const auto* node = dynamic_cast<const papyrus::AppNodeDescriptor*>(object)) {
            visit(node->child.get(), depth + 1);
            return;
        }
        const auto* shape = dynamic_cast<const papyrus::ShapeDescriptor*>(object);
        const auto* geometry = shape
            ? dynamic_cast<const papyrus::GeometryDescriptor*>(shape->geometry.get())
            : dynamic_cast<const papyrus::GeometryDescriptor*>(object);
        if (!geometry) return;
        const auto* vertices = dynamic_cast<const papyrus::PlainVertexListDescriptor*>(
            geometry->vertex_list.get());
        if (!vertices || vertices->num_vertices <= 0) return;
        const auto count = static_cast<std::size_t>(vertices->num_vertices);
        if (vertices->positions_x.size() < count || vertices->positions_y.size() < count ||
            vertices->positions_z.size() < count) return;
        const auto* appearance = shape
            ? dynamic_cast<const papyrus::AppearanceDescriptor*>(shape->appearance.get())
            : nullptr;
        const auto* texture = appearance
            ? dynamic_cast<const papyrus::TextureDescriptor*>(
                  appearance->texture_slots[0].get())
            : nullptr;
        const std::int32_t texture_index = texture
            ? resources.texture(texture->texture_name) : -1;
        const std::vector<double>* u = vertices->uv_channels.size() > 0
            ? &vertices->uv_channels[0] : nullptr;
        const std::vector<double>* v = vertices->uv_channels.size() > 1
            ? &vertices->uv_channels[1] : nullptr;
        for (const papyrus::PersistentObject* primitive = geometry->primitive.get();
             primitive; primitive = next_primitive(primitive)) {
            auto indices = triangle_indices(primitive);
            if (indices.empty()) continue;
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.reserve(mesh.vertices.size() + count);
            for (std::size_t i = 0; i < count; ++i) {
                const Vec3 position = transform_point(transform, {
                    static_cast<float>(vertices->positions_x[i]),
                    static_cast<float>(vertices->positions_y[i]),
                    static_cast<float>(vertices->positions_z[i]),
                });
                mesh.vertices.push_back({position,
                    u && i < u->size() ? static_cast<float>((*u)[i]) : 0.0f,
                    v && i < v->size() ? static_cast<float>((*v)[i]) : 0.0f});
            }
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                if (indices[i] >= count || indices[i + 1] >= count ||
                    indices[i + 2] >= count) continue;
                mesh.triangles.push_back({base + indices[i], base + indices[i + 1],
                    base + indices[i + 2], 0xffffff, 0, texture_index});
            }
        }
    };
    visit(root.get(), 0);
    return mesh;
}

void append_scene_instance(TrackViewModel& model, const TrackMesh& asset,
                           const Matrix4& transform,
                           const TrackBounds& track_bounds) {
    if (asset.vertices.empty() || asset.triangles.empty()) return;
    const auto base = static_cast<std::uint32_t>(model.scenery.vertices.size());
    const Vec3 fit_center = track_bounds.center();
    const float maximum_distance = std::max(100.0f, track_bounds.radius() * 6.0f);
    std::vector<bool> valid;
    valid.reserve(asset.vertices.size());
    model.scenery.vertices.reserve(model.scenery.vertices.size() + asset.vertices.size());
    for (const auto& vertex : asset.vertices) {
        const Vec3 position = transform_point(transform, vertex.position);
        const Vec3 offset = position - fit_center;
        const bool usable = std::isfinite(position.x) && std::isfinite(position.y) &&
            std::isfinite(position.z) && offset.length() <= maximum_distance;
        valid.push_back(usable);
        model.scenery.vertices.push_back({position, vertex.u, vertex.v});
    }
    model.scenery.triangles.reserve(
        model.scenery.triangles.size() + asset.triangles.size());
    for (auto triangle : asset.triangles) {
        if (triangle.a >= valid.size() || triangle.b >= valid.size() ||
            triangle.c >= valid.size() || !valid[triangle.a] ||
            !valid[triangle.b] || !valid[triangle.c]) continue;
        triangle.a += base;
        triangle.b += base;
        triangle.c += base;
        model.scenery.triangles.push_back(triangle);
    }
}

struct ProjectedVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    bool visible = false;
};

struct RasterContext {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t>* pixels = nullptr;
    std::vector<float> depth;
    Vec3 eye{};
    Vec3 right{};
    Vec3 up{};
    Vec3 forward{};
    float focal = 1.0f;
};

Vec3 normalized(Vec3 value) {
    const float length = value.length();
    return length > 1.0e-8f ? value * (1.0f / length) : Vec3{};
}

ProjectedVertex project(const RasterContext& context, Vec3 point) {
    const Vec3 relative = point - context.eye;
    const float z = dot(relative, context.forward);
    if (z <= 0.01f) return {};
    return {
        context.width * 0.5f + dot(relative, context.right) * context.focal / z,
        context.height * 0.5f - dot(relative, context.up) * context.focal / z,
        z,
        0.0f,
        0.0f,
        true,
    };
}

std::uint32_t scale_color(std::uint32_t color, float scale) {
    const auto channel = [&](int shift) {
        return static_cast<std::uint32_t>(std::clamp(
            static_cast<int>(((color >> shift) & 0xffu) * scale), 0, 255));
    };
    return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

void raster_triangle(RasterContext& context,
                     const ProjectedVertex& a,
                     const ProjectedVertex& b,
                     const ProjectedVertex& c,
                     std::uint32_t color,
                     const TrackTexture* texture,
                     float lighting) {
    const float area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
    if (std::fabs(area) < 1.0e-4f) return;
    const float min_xf = std::max(0.0f, std::min({a.x, b.x, c.x}));
    const float max_xf = std::min(static_cast<float>(context.width - 1),
                                  std::max({a.x, b.x, c.x}));
    const float min_yf = std::max(0.0f, std::min({a.y, b.y, c.y}));
    const float max_yf = std::min(static_cast<float>(context.height - 1),
                                  std::max({a.y, b.y, c.y}));
    if (min_xf > max_xf || min_yf > max_yf) return;
    const int min_x = static_cast<int>(std::floor(min_xf));
    const int max_x = static_cast<int>(std::ceil(max_xf));
    const int min_y = static_cast<int>(std::floor(min_yf));
    const int max_y = static_cast<int>(std::ceil(max_yf));
    const float inverse_area = 1.0f / area;
    const float inverse_z[3] = {1.0f / a.z, 1.0f / b.z, 1.0f / c.z};
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const float px = x + 0.5f;
            const float py = y + 0.5f;
            float w0 = edge(b.x, b.y, c.x, c.y, px, py) * inverse_area;
            float w1 = edge(c.x, c.y, a.x, a.y, px, py) * inverse_area;
            float w2 = edge(a.x, a.y, b.x, b.y, px, py) * inverse_area;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            const float inverse_depth = w0 * inverse_z[0] +
                                        w1 * inverse_z[1] +
                                        w2 * inverse_z[2];
            if (inverse_depth <= 0.0f) continue;
            const float z = 1.0f / inverse_depth;
            const std::size_t index = static_cast<std::size_t>(y) * context.width + x;
            if (z >= context.depth[index]) continue;
            std::uint32_t pixel = color;
            std::uint32_t alpha = 255;
            if (texture && texture->width > 0 && texture->height > 0 &&
                texture->rgba.size() >=
                    static_cast<std::size_t>(texture->width) * texture->height * 4u) {
                float u = (w0 * a.u * inverse_z[0] +
                           w1 * b.u * inverse_z[1] +
                           w2 * c.u * inverse_z[2]) / inverse_depth;
                float v = (w0 * a.v * inverse_z[0] +
                           w1 * b.v * inverse_z[1] +
                           w2 * c.v * inverse_z[2]) / inverse_depth;
                auto address = [](float coordinate, bool clamp) {
                    if (clamp) return std::clamp(coordinate, 0.0f, 1.0f);
                    coordinate -= std::floor(coordinate);
                    return coordinate < 0.0f ? coordinate + 1.0f : coordinate;
                };
                u = address(u, texture->clamp_u);
                v = address(v, texture->clamp_v);
                const auto tx = static_cast<std::uint32_t>(std::clamp(
                    static_cast<int>(u * (texture->width - 1) + 0.5f),
                    0, static_cast<int>(texture->width - 1)));
                const auto ty = static_cast<std::uint32_t>(std::clamp(
                    static_cast<int>(v * (texture->height - 1) + 0.5f),
                    0, static_cast<int>(texture->height - 1)));
                const auto source = (static_cast<std::size_t>(ty) * texture->width + tx) * 4u;
                alpha = texture->rgba[source + 3];
                if (alpha < 96) continue;
                pixel = (static_cast<std::uint32_t>(texture->rgba[source]) << 16) |
                        (static_cast<std::uint32_t>(texture->rgba[source + 1]) << 8) |
                        texture->rgba[source + 2];
            }
            context.depth[index] = z;
            pixel = scale_color(pixel, lighting);
            if (alpha < 250) {
                const std::uint32_t destination = (*context.pixels)[index];
                const auto blend = [&](int shift) {
                    return ((((pixel >> shift) & 0xffu) * alpha +
                             ((destination >> shift) & 0xffu) * (255u - alpha)) / 255u)
                           << shift;
                };
                pixel = blend(16) | blend(8) | blend(0);
            }
            (*context.pixels)[index] = pixel;
        }
    }
}

void raster_line(RasterContext& context,
                 ProjectedVertex a, ProjectedVertex b,
                 std::uint32_t color, bool depth_test) {
    if (!a.visible || !b.visible) return;
    const float original_dx = b.x - a.x;
    const float original_dy = b.y - a.y;
    float first = 0.0f;
    float last = 1.0f;
    const auto clip = [&](float p, float q) {
        if (std::fabs(p) < 1.0e-12f) return q >= 0.0f;
        const float value = q / p;
        if (p < 0.0f) {
            if (value > last) return false;
            first = std::max(first, value);
        } else {
            if (value < first) return false;
            last = std::min(last, value);
        }
        return true;
    };
    if (!clip(-original_dx, a.x) ||
        !clip(original_dx, context.width - 1.0f - a.x) ||
        !clip(-original_dy, a.y) ||
        !clip(original_dy, context.height - 1.0f - a.y) || first > last) return;
    const float inverse_a = 1.0f / a.z;
    const float inverse_b = 1.0f / b.z;
    const auto clipped = [&](float t) {
        ProjectedVertex point;
        point.x = a.x + original_dx * t;
        point.y = a.y + original_dy * t;
        point.z = 1.0f / (inverse_a + (inverse_b - inverse_a) * t);
        point.visible = true;
        return point;
    };
    const ProjectedVertex clipped_a = clipped(first);
    const ProjectedVertex clipped_b = clipped(last);
    a = clipped_a;
    b = clipped_b;
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const int steps = std::max(1, static_cast<int>(std::ceil(
        std::max(std::fabs(dx), std::fabs(dy)))));
    for (int step = 0; step <= steps; ++step) {
        const float t = static_cast<float>(step) / steps;
        const int x = static_cast<int>(std::lround(a.x + dx * t));
        const int y = static_cast<int>(std::lround(a.y + dy * t));
        if (x < 0 || y < 0 || x >= context.width || y >= context.height) continue;
        const float inverse_depth = (1.0f - t) / a.z + t / b.z;
        const float z = inverse_depth > 0.0f ? 1.0f / inverse_depth
                                             : std::numeric_limits<float>::infinity();
        const std::size_t index = static_cast<std::size_t>(y) * context.width + x;
        if (depth_test && z > context.depth[index] + 0.5f) continue;
        if (z < context.depth[index]) context.depth[index] = z;
        (*context.pixels)[index] = color;
    }
}

void render_solid_mesh(RasterContext& context, const TrackMesh& mesh,
                       const std::vector<TrackTexture>& textures) {
    std::vector<ProjectedVertex> projected;
    projected.reserve(mesh.vertices.size());
    for (const auto& vertex : mesh.vertices) {
        auto point = project(context, vertex.position);
        point.u = vertex.u;
        point.v = vertex.v;
        projected.push_back(point);
    }
    const Vec3 light = normalized(Vec3{-0.35f, -0.25f, 1.0f});
    for (const auto& triangle : mesh.triangles) {
        if (triangle.a >= projected.size() || triangle.b >= projected.size() ||
            triangle.c >= projected.size()) continue;
        const auto& a = projected[triangle.a];
        const auto& b = projected[triangle.b];
        const auto& c = projected[triangle.c];
        if (!a.visible || !b.visible || !c.visible) continue;
        const Vec3 va = mesh.vertices[triangle.a].position;
        const Vec3 vb = mesh.vertices[triangle.b].position;
        const Vec3 vc = mesh.vertices[triangle.c].position;
        const Vec3 normal = normalized(cross(vb - va, vc - va));
        const float lighting = 0.36f + 0.64f * std::fabs(dot(normal, light));
        const TrackTexture* texture = triangle.texture >= 0 &&
            static_cast<std::size_t>(triangle.texture) < textures.size()
            ? &textures[static_cast<std::size_t>(triangle.texture)] : nullptr;
        raster_triangle(context, a, b, c, triangle.color, texture, lighting);
    }
}

void render_wire_mesh(RasterContext& context, const TrackMesh& mesh,
                      std::uint32_t color) {
    std::vector<ProjectedVertex> projected;
    projected.reserve(mesh.vertices.size());
    for (const auto& vertex : mesh.vertices) {
        projected.push_back(project(context, vertex.position));
    }
    if (!mesh.wire_lines.empty()) {
        for (const auto& line : mesh.wire_lines) {
            if (line.a >= projected.size() || line.b >= projected.size()) continue;
            raster_line(context, projected[line.a], projected[line.b], color, true);
        }
        return;
    }
    for (const auto& triangle : mesh.triangles) {
        if (triangle.a >= projected.size() || triangle.b >= projected.size() ||
            triangle.c >= projected.size()) continue;
        raster_line(context, projected[triangle.a], projected[triangle.b], color, true);
        raster_line(context, projected[triangle.b], projected[triangle.c], color, true);
        raster_line(context, projected[triangle.c], projected[triangle.a], color, true);
    }
}

void render_centerline(RasterContext& context, const std::vector<Vec3>& points,
                       std::uint32_t color) {
    if (points.size() < 2) return;
    for (std::size_t i = 0; i < points.size(); ++i) {
        raster_line(context, project(context, points[i]),
                    project(context, points[(i + 1) % points.size()]), color, false);
    }
}

void append_u16(std::ofstream& output, std::uint16_t value) {
    output.put(static_cast<char>(value));
    output.put(static_cast<char>(value >> 8));
}

void append_u32(std::ofstream& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.put(static_cast<char>(value >> shift));
    }
}

}  // namespace

Vec3 TrackBounds::center() const {
    return valid ? (minimum + maximum) * 0.5f : Vec3{};
}

float TrackBounds::radius() const {
    return valid ? (maximum - minimum).length() * 0.5f : 1.0f;
}

TrackViewModel load_track_view(
        const std::filesystem::path& path,
        const std::optional<std::filesystem::path>& shared_folder) {
    const PtfSource source = load_ptf_source(path, shared_folder);
    TrackViewModel model;
    model.source_path = source.source_path;
    model.source_description = source.description;
    for (const auto& layer : source.resources->layers) {
        if (layer.archive) {
            model.resource_search_order.push_back(
                "DAT: " + layer.archive->path.string());
        }
        model.resource_search_order.push_back("Folder: " + layer.folder.string());
    }
    auto parsed = parse_track(source.bytes);
    model.segment_count = parsed.declared_segment_count;
    ModelResourceLoader resources(model, source.resources);

    std::vector<std::vector<ProfilePoint>> join_profiles(parsed.segments.size());
    for (std::size_t i = 0; i < parsed.segments.size(); ++i) {
        const std::size_t next = (i + 1) % parsed.segments.size();
        join_profiles[i] = shared_join_profile(
            profile_at(parsed.segments[i], 1.0f),
            profile_at(parsed.segments[next], 0.0f));
    }

    for (std::size_t segment_index = 0;
         segment_index < parsed.segments.size(); ++segment_index) {
        const auto& segment = parsed.segments[segment_index];
        const auto& start_profile = join_profiles[
            (segment_index + parsed.segments.size() - 1) % parsed.segments.size()];
        const auto& end_profile = join_profiles[segment_index];
        model.material_count += static_cast<std::uint32_t>(segment.materials.size());
        model.wall_count += static_cast<std::uint32_t>(segment.walls.size());
        const int subdivisions = segment_subdivisions(segment);
        for (int step = 0; step < subdivisions; ++step) {
            const float t0 = static_cast<float>(step) / subdivisions;
            const float t1 = static_cast<float>(step + 1) / subdivisions;
            const auto* profile0 = step == 0 ? &start_profile : nullptr;
            const auto* profile1 = step + 1 == subdivisions ? &end_profile : nullptr;
            build_surface_step(model, segment, t0, t1, resources,
                               profile0, profile1);
            for (const auto& wall : segment.walls) {
                add_wall_step(model.visual_walls, model.bounds, segment, wall,
                              t0, t1, false, resources, profile0, profile1);
                add_wall_step(model.collision_walls, model.bounds, segment, wall,
                              t0, t1, true, resources, profile0, profile1);
            }
            Vec3 center;
            float heading = 0.0f;
            sample_segment(segment, t0, center, heading);
            center.z = height_at(profile0 ? *profile0 : profile_at(segment, t0), 0.0f);
            model.centerline.push_back(center);
        }
    }

    std::unordered_map<std::string, TrackMesh> asset_cache;
    std::unordered_set<std::string> missing_assets;
    std::unordered_map<std::string, std::size_t> missing_object_indices;
    const auto mark_missing_object = [&](const std::string& key,
                                         const std::string& name,
                                         const std::string& reason) {
        ++model.missing_object_count;
        if (const auto found = missing_object_indices.find(key);
            found != missing_object_indices.end()) {
            ++model.missing_objects[found->second].instance_count;
            return;
        }
        missing_object_indices.emplace(key, model.missing_objects.size());
        model.missing_objects.push_back({name, reason, 1});
    };
    const TrackBounds track_bounds = model.bounds;
    for (const auto& instance : parsed.objects) {
        const std::string key = resource_key(instance.name);
        if (missing_assets.contains(key)) {
            mark_missing_object(key, instance.name, {});
            continue;
        }
        auto found = asset_cache.find(key);
        if (found == asset_cache.end()) {
            try {
                const auto bytes = source.resources->read(instance.name);
                if (!bytes) {
                    missing_assets.insert(key);
                    mark_missing_object(key, instance.name,
                                        "resource was not found");
                    continue;
                }
                auto asset = load_scene_asset(*bytes, resources);
                if (asset.triangles.empty()) {
                    missing_assets.insert(key);
                    mark_missing_object(key, instance.name,
                                        "the 3DO produced no triangles");
                    continue;
                }
                found = asset_cache.emplace(key, std::move(asset)).first;
            } catch (const std::exception& error) {
                missing_assets.insert(key);
                mark_missing_object(key, instance.name, error.what());
                continue;
            }
        }
        append_scene_instance(model, found->second, make_transform(
            instance.transform[0], instance.transform[1], instance.transform[2],
            instance.transform[3], instance.transform[4], instance.transform[5]),
            track_bounds);
        ++model.object_count;
    }

    if (model.surface.triangles.empty()) {
        throw std::runtime_error("the PTF did not produce a surface mesh");
    }
    return model;
}

TrackFrame render_track_view(const TrackViewModel& model,
                             const TrackCamera& camera,
                             TrackViewModes modes,
                             int width,
                             int height) {
    TrackFrame frame;
    frame.width = std::max(1, width);
    frame.height = std::max(1, height);
    frame.pixels.resize(static_cast<std::size_t>(frame.width) * frame.height);
    for (int y = 0; y < frame.height; ++y) {
        const float t = static_cast<float>(y) / std::max(1, frame.height - 1);
        const std::uint32_t c = has_track_view_mode(modes, TrackViewMode::Solid)
            ? scale_color(0x16202b, 1.0f - 0.35f * t) : 0x0b0e12;
        std::fill(frame.pixels.begin() + static_cast<std::ptrdiff_t>(y) * frame.width,
                  frame.pixels.begin() + static_cast<std::ptrdiff_t>(y + 1) * frame.width,
                  c);
    }

    RasterContext context;
    context.width = frame.width;
    context.height = frame.height;
    context.pixels = &frame.pixels;
    context.depth.assign(frame.pixels.size(), std::numeric_limits<float>::infinity());
    const float radius = std::max(model.bounds.radius(), 10.0f);
    const float zoom = std::clamp(camera.zoom, 0.08f, 20.0f);
    const float distance = radius * 1.85f / zoom;
    const float cp = std::cos(camera.pitch);
    const Vec3 direction{std::cos(camera.yaw) * cp,
                         std::sin(camera.yaw) * cp,
                         std::sin(camera.pitch)};
    context.eye = camera.target + direction * distance;
    context.forward = normalized(camera.target - context.eye);
    context.right = normalized(cross(context.forward, Vec3{0.0f, 0.0f, 1.0f}));
    if (context.right.length() < 1.0e-5f) context.right = {1.0f, 0.0f, 0.0f};
    context.up = normalized(cross(context.right, context.forward));
    const float fov = 52.0f * kPi / 180.0f;
    context.focal = 0.5f * static_cast<float>(std::min(frame.width, frame.height)) /
                    std::tan(fov * 0.5f);

    if (has_track_view_mode(modes, TrackViewMode::Solid)) {
        render_solid_mesh(context, model.surface, model.textures);
        render_solid_mesh(context, model.visual_walls, model.textures);
        render_solid_mesh(context, model.scenery, model.textures);
    }
    if (has_track_view_mode(modes, TrackViewMode::Wireframe)) {
        render_wire_mesh(context, model.surface, 0x69c7d8);
        render_wire_mesh(context, model.visual_walls, 0xc5d0d8);
        render_wire_mesh(context, model.scenery, 0x83a58f);
    }
    if (has_track_view_mode(modes, TrackViewMode::Collision)) {
        render_wire_mesh(context, model.surface, 0xe0b64c);
        render_wire_mesh(context, model.collision_walls, 0xe05a52);
    }
    const std::uint32_t centerline_color =
        has_track_view_mode(modes, TrackViewMode::Collision) ? 0x52c7d8 :
        has_track_view_mode(modes, TrackViewMode::Wireframe) ? 0xf0d35c : 0x4fb8d1;
    render_centerline(context, model.centerline, centerline_color);
    return frame;
}

void write_track_view_bmp(const std::filesystem::path& path,
                          const TrackFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.pixels.size() != static_cast<std::size_t>(frame.width) * frame.height) {
        throw std::runtime_error("invalid track frame");
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot create " + path.string());
    const std::uint32_t pixel_bytes = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(frame.width) * frame.height * 4u);
    output.write("BM", 2);
    append_u32(output, 14u + 40u + pixel_bytes);
    append_u32(output, 0);
    append_u32(output, 54);
    append_u32(output, 40);
    append_u32(output, static_cast<std::uint32_t>(frame.width));
    append_u32(output, static_cast<std::uint32_t>(frame.height));
    append_u16(output, 1);
    append_u16(output, 32);
    append_u32(output, 0);
    append_u32(output, pixel_bytes);
    append_u32(output, 2835);
    append_u32(output, 2835);
    append_u32(output, 0);
    append_u32(output, 0);
    for (int y = frame.height - 1; y >= 0; --y) {
        const auto* row = frame.pixels.data() + static_cast<std::size_t>(y) * frame.width;
        output.write(reinterpret_cast<const char*>(row),
                     static_cast<std::streamsize>(frame.width * 4));
    }
    if (!output) throw std::runtime_error("cannot write " + path.string());
}

}  // namespace opennr::viewer
