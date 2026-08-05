// object_tool: walk a Papyrus .3do or .ptf file, print structure.
//
// Reads either an extracted file directly, or a (.dat, entry-name)
// pair so it can decompress on the fly. When `--tree` is passed it
// prints the decoded descriptor tree (typed payloads); `--scene-states`
// prints the exact named selector threshold tables used by SceneView;
// `--animations` prints every authored AnimatedTransform channel and its
// timestamp range;
// otherwise it prints the raw token list produced by the structural walker.

#include "fs/dat_archive.h"
#include "fs/descriptors.h"
#include "fs/object_stream.h"
#include "fs/object_tree.h"
#include "fs/papyrus_archive.h"
#include "fs/papyrus_descriptors.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

static std::vector<std::uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    auto sz = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::uint8_t> b(sz);
    f.read(reinterpret_cast<char*>(b.data()), sz);
    return b;
}

static void print_descriptor(const opennr::ObjectNode& node, int indent) {
    auto pad = [&]() { for (int i = 0; i < indent; ++i) std::putchar(' '); };
    pad();
    std::printf("<%s>", node.class_name.c_str());

    using namespace opennr;
    std::visit([&](auto const& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            // no decoded payload
        } else {
            std::printf(" {");
            if constexpr (requires { payload.header; }) {
                if (!payload.header.name.empty()) {
                    std::printf(" name=\"%s\"", payload.header.name.c_str());
                }
            }
            if constexpr (std::is_same_v<T, GroupDescriptor>) {
                std::printf(" num_children=%u", payload.num_children);
            } else if constexpr (std::is_same_v<T, GroupingNodeDescriptor>) {
                std::printf(" num_children=%u", payload.num_children);
            } else if constexpr (std::is_same_v<T, LodSwitchDescriptor>) {
                std::printf(" num_levels=%d", payload.num_lod_levels);
            } else if constexpr (std::is_same_v<T, StateSwitchDescriptor>) {
                std::printf(" num_states=%d", payload.num_states);
            } else if constexpr (std::is_same_v<T, TransformDescriptor>) {
                if (payload.fields_decoded) {
                    std::printf(" t=(%g,%g,%g) ypr=(%g,%g,%g)",
                                payload.tx, payload.ty, payload.tz,
                                payload.yaw, payload.pitch, payload.roll);
                }
            } else if constexpr (std::is_same_v<T, AnimatedTransformDescriptor>) {
                if (payload.fields_decoded) {
                    std::printf(" t=(%g,%g,%g) axis=(%g,%g,%g)",
                                payload.tx, payload.ty, payload.tz,
                                payload.axis_x, payload.axis_y, payload.axis_z);
                }
            } else if constexpr (std::is_same_v<T, ProgressiveModificationDescriptor>) {
                std::printf(" dv=%d mv=%d dt=%d mt=%d",
                            payload.change_num_vertices,
                            payload.num_modified_vertices,
                            payload.change_num_tris,
                            payload.num_modified_tris);
            } else if constexpr (std::is_same_v<T, TrackDescriptor>) {
                std::printf(" type=%u num_segments=%d",
                            payload.type_code, payload.num_segments);
            } else if constexpr (std::is_same_v<T, GeometryDescriptor>) {
                std::printf(" type=%u unk=(%u,%u)",
                            payload.type_code, payload.unk_a, payload.unk_b);
            } else if constexpr (std::is_same_v<T, TextureDescriptor>) {
                std::printf(" type=%u tex=\"%s\"",
                            payload.type_code, payload.texture_name.c_str());
                if (payload.uv_matrix.size() == 12) std::printf(" uv=12d");
            } else if constexpr (std::is_same_v<T, PlainVertexListDescriptor>) {
                std::printf(" num_vertices=%d", payload.num_vertices);
            } else if constexpr (std::is_same_v<T, TriStripDescriptor>) {
                std::printf(" num_indices=%d", payload.num_indices);
            } else if constexpr (std::is_same_v<T, TriListDescriptor>) {
                std::printf(" num_indices=%d", payload.num_indices);
            } else if constexpr (std::is_same_v<T, TriFanDescriptor>) {
                std::printf(" num_indices=%d", payload.num_indices);
            }
            std::printf(" }");
        }
    }, node.descriptor);
    std::putchar('\n');
    for (const auto& c : node.children) {
        if (c) print_descriptor(*c, indent + 2);
    }
}

static void print_animations(
    const std::shared_ptr<opennr::papyrus::PersistentObject>& object,
    int indent,
    std::unordered_set<const opennr::papyrus::PersistentObject*>& seen) {
    using namespace opennr::papyrus;
    if (!object || !seen.insert(object.get()).second) return;
    const auto pad = [&]() { for (int i = 0; i < indent; ++i) std::putchar(' '); };
    if (const auto* animated = dynamic_cast<const AnimatedTransformDescriptor*>(object.get())) {
        pad();
        std::printf("AnimatedTransform name=\"%s\" channel=\"%s\" keys=%u",
                    animated->header.name.c_str(), animated->channel_name.c_str(),
                    animated->num_keyframes);
        if (!animated->keyframes.empty()) {
            std::printf(" time=[%u,%u]", animated->keyframes.front().timestamp,
                        animated->keyframes.back().timestamp);
        }
        std::putchar('\n');
        print_animations(animated->child, indent + 2, seen);
        return;
    }
    if (const auto* transform = dynamic_cast<const TransformDescriptor*>(object.get())) {
        if (!transform->header.name.empty()) {
            pad();
            std::printf("Transform name=\"%s\" t=(%g,%g,%g) ypr=(%g,%g,%g)\n",
                        transform->header.name.c_str(), transform->tx,
                        transform->ty, transform->tz, transform->yaw,
                        transform->pitch, transform->roll);
        }
        print_animations(transform->child, indent, seen);
        return;
    }
    if (const auto* group = dynamic_cast<const GroupDescriptor*>(object.get())) {
        for (const auto& child : group->children) print_animations(child, indent, seen);
        return;
    }
    if (const auto* grouping = dynamic_cast<const GroupingNodeDescriptor*>(object.get())) {
        for (const auto& child : grouping->children) print_animations(child, indent, seen);
        return;
    }
    if (const auto* state = dynamic_cast<const StateSwitchDescriptor*>(object.get())) {
        for (const auto& child : state->children) print_animations(child, indent, seen);
        return;
    }
    if (const auto* lod = dynamic_cast<const LodSwitchDescriptor*>(object.get())) {
        for (const auto& child : lod->children) print_animations(child, indent, seen);
        return;
    }
    if (const auto* billboard = dynamic_cast<const BillboardDescriptor*>(object.get())) {
        print_animations(billboard->child, indent, seen);
        return;
    }
    if (const auto* app = dynamic_cast<const AppNodeDescriptor*>(object.get())) {
        print_animations(app->child, indent, seen);
        return;
    }
    if (const auto* shape = dynamic_cast<const ShapeDescriptor*>(object.get())) {
        if (!shape->header.name.empty()) {
            pad();
            std::printf("Shape name=\"%s\"\n", shape->header.name.c_str());
        }
    }
}

// Print the named scalar switches which drive a renderable typed-object
// asset.  This is intentionally separate from ObjectTree's structural dump:
// SceneView consumes the richer papyrus:: descriptor classes, whose switches
// retain the exact state-name and threshold table.
static void print_scene_switches(
    const std::shared_ptr<opennr::papyrus::PersistentObject>& object,
    int indent,
    std::unordered_set<const opennr::papyrus::PersistentObject*>& seen) {
    using namespace opennr::papyrus;
    if (!object || !seen.insert(object.get()).second) return;
    const auto pad = [&] { for (int i = 0; i < indent; ++i) std::putchar(' '); };
    const auto visit_children = [&](const auto& children, int child_indent) {
        for (const auto& child : children)
            print_scene_switches(child, child_indent, seen);
    };

    if (const auto* state = dynamic_cast<const StateSwitchDescriptor*>(object.get())) {
        pad();
        std::printf("StateSwitch name=\"%s\" states=%u default=%g thresholds=",
                    state->state_name.c_str(), state->num_states,
                    state->default_value);
        for (std::size_t i = 0; i < state->state_values.size(); ++i)
            std::printf("%s%g", i == 0 ? "[" : ", ", state->state_values[i]);
        std::printf("]\n");
        for (std::size_t i = 0; i < state->children.size(); ++i) {
            pad();
            if (i + 1 < state->state_values.size()) {
                std::printf("  child[%zu] range=[%g, %g]\n", i,
                            state->state_values[i], state->state_values[i + 1]);
            } else {
                std::printf("  child[%zu]\n", i);
            }
            print_scene_switches(state->children[i], indent + 4, seen);
        }
        return;
    }
    if (const auto* lod = dynamic_cast<const LodSwitchDescriptor*>(object.get())) {
        visit_children(lod->children, indent);
    } else if (const auto* group = dynamic_cast<const GroupDescriptor*>(object.get())) {
        visit_children(group->children, indent);
    } else if (const auto* grouping = dynamic_cast<const GroupingNodeDescriptor*>(object.get())) {
        visit_children(grouping->children, indent);
    } else if (const auto* transform = dynamic_cast<const TransformDescriptor*>(object.get())) {
        print_scene_switches(transform->child, indent, seen);
    } else if (const auto* animated = dynamic_cast<const AnimatedTransformDescriptor*>(object.get())) {
        print_scene_switches(animated->child, indent, seen);
    } else if (const auto* billboard = dynamic_cast<const BillboardDescriptor*>(object.get())) {
        print_scene_switches(billboard->child, indent, seen);
    } else if (const auto* app = dynamic_cast<const AppNodeDescriptor*>(object.get())) {
        print_scene_switches(app->child, indent, seen);
    } else if (const auto* portal = dynamic_cast<const PortalDescriptor*>(object.get())) {
        print_scene_switches(portal->target, indent, seen);
    } else if (const auto* shape = dynamic_cast<const ShapeDescriptor*>(object.get())) {
        print_scene_switches(shape->appearance, indent, seen);
        print_scene_switches(shape->geometry, indent, seen);
    } else if (const auto* appearance = dynamic_cast<const AppearanceDescriptor*>(object.get())) {
        for (const auto& slot : appearance->texture_slots)
            print_scene_switches(slot, indent, seen);
    } else if (const auto* geometry = dynamic_cast<const GeometryDescriptor*>(object.get())) {
        print_scene_switches(geometry->vertex_list, indent, seen);
        print_scene_switches(geometry->primitive, indent, seen);
    } else if (const auto* tri = dynamic_cast<const TriListDescriptor*>(object.get())) {
        print_scene_switches(tri->next_primitive, indent, seen);
    } else if (const auto* strip = dynamic_cast<const TriStripDescriptor*>(object.get())) {
        print_scene_switches(strip->next_primitive, indent, seen);
    } else if (const auto* fan = dynamic_cast<const TriFanDescriptor*>(object.get())) {
        print_scene_switches(fan->next_primitive, indent, seen);
    }
}

int main(int argc, char** argv) {
    bool tree_mode = false;
    bool scene_states_mode = false;
    bool animations_mode = false;
    int  argi = 1;
    while (argi < argc) {
        if (std::strcmp(argv[argi], "--tree") == 0) {
            tree_mode = true; ++argi;
        } else if (std::strcmp(argv[argi], "--scene-states") == 0) {
            scene_states_mode = true; ++argi;
        } else if (std::strcmp(argv[argi], "--animations") == 0) {
            animations_mode = true; ++argi;
        } else {
            break;
        }
    }
    if (argc - argi < 1) {
        std::fprintf(stderr,
            "usage: %s [--tree|--animations] file.3do|file.ptf\n"
            "       %s [--tree|--animations] archive.dat entry-name\n"
            "       %s --scene-states file.3do|file.ptf\n"
            "       %s --scene-states archive.dat entry-name\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    try {
        std::vector<std::uint8_t> bytes;
        if (argc - argi == 1) {
            bytes = read_file(argv[argi]);
        } else {
            auto arc = opennr::DatArchive::load(argv[argi]);
            const opennr::DatEntry* found = nullptr;
            for (const auto& e : arc.entries()) {
                if (e.name == argv[argi + 1]) { found = &e; break; }
            }
            if (!found) {
                std::fprintf(stderr, "entry '%s' not found in %s\n",
                             argv[argi + 1], argv[argi]);
                return 1;
            }
            bytes = arc.read(*found);
        }
        if (scene_states_mode) {
            opennr::papyrus::register_all_descriptors();
            opennr::papyrus::Archive ar{bytes};
            const auto root = ar.read_object();
            std::printf("file: %zu bytes; scene state switches:\n", bytes.size());
            std::unordered_set<const opennr::papyrus::PersistentObject*> seen;
            print_scene_switches(root, 2, seen);
            return 0;
        }
        if (animations_mode) {
            opennr::papyrus::register_all_descriptors();
            opennr::papyrus::Archive ar{bytes};
            const auto root = ar.read_object();
            std::unordered_set<const opennr::papyrus::PersistentObject*> seen;
            print_animations(root, 0, seen);
            return 0;
        }
        if (tree_mode) {
            auto t = opennr::ObjectTree::parse(bytes);
            std::printf("file: %zu bytes; header u32 %u %u\n",
                        bytes.size(),
                        t.stream_version_a, t.stream_version_b);
            std::printf("class counts: %zu distinct\n", t.class_counts.size());
            if (t.root) print_descriptor(*t.root, 0);
            return 0;
        }
        auto s = opennr::ObjectStream::parse(bytes);
        std::printf("file: %zu bytes; header u32 %u %u\n",
                    bytes.size(), s.stream_version_a, s.stream_version_b);
        std::printf("tokens: %zu (classes %zu)\n", s.tokens.size(),
                    s.classes_in_order().size());
        for (const auto& t : s.tokens) {
            const char* tag = t.is_class ? "CLASS" : "name ";
            std::printf("  @0x%08zx  %s len=%3u  '%s'\n",
                        t.offset, tag, t.length, t.name.c_str());
        }
        auto tex = s.texture_refs();
        if (!tex.empty()) {
            std::printf("\ntexture refs (%zu):\n", tex.size());
            for (const auto& m : tex) std::printf("  %s\n", m.c_str());
        }
        auto objs = s.object_refs();
        if (!objs.empty()) {
            std::printf("\n.3do refs (%zu):\n", objs.size());
            for (const auto& m : objs) std::printf("  %s\n", m.c_str());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
