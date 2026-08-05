// rpy_tool — dump and validate a .rpy replay file.
//
// Usage:
//   opennr_rpy_tool <path_to_replay.rpy>
//   opennr_rpy_tool --validate <path>     (exit 0 only if internally consistent)
//   opennr_rpy_tool --dump-type14 <path>  (dump tire-mark quads)
//   opennr_rpy_tool --dump-type11 <path>  (dump 3-D debris/panel states)
//   opennr_rpy_tool --event-counts <path> (count every RPTP event type)
//   opennr_rpy_tool --dump-editor <path>  (dump raw replay-editor records)
//   opennr_rpy_tool --make-type11-follow <in> <out> <first-block>
//                       <block-count> <car-slot> [panel-id]
//
// Reports header, driver roster, lap-time matrix, and per-frame marker
// statistics. The internal-consistency checks cover:
//   - RPHD's `frame_count` vs. the scanned `marker_count`
//   - RPHD's `rptp_body_size` vs. the actual RPTP chunk size
//   - LPTB lap-time monotonicity (each car's recorded splits ascend)
//   - DRNT car-number plausibility (1..199 in stock NASCAR)
//
// Exits 0 on success, 1 on parse failure, 2 on validation failure.

#include "fs/rpy_editor.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<std::uint8_t> v(static_cast<std::size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(v.data()), sz);
    return v;
}

bool parse_u32(const char* text, std::uint32_t& out) {
    const auto first = text;
    const auto last = text + std::strlen(text);
    const auto result = std::from_chars(first, last, out, 10);
    return result.ec == std::errc{} && result.ptr == last;
}

bool write_all_new_file(const std::filesystem::path& path,
                        std::span<const std::uint8_t> bytes) {
    std::error_code error;
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return bool(file);
}

int make_type11_follow_fixture(int argc, char** argv) {
    if (argc != 7 && argc != 8) {
        std::fprintf(stderr,
            "usage: %s --make-type11-follow <in.rpy> <out.rpy> <first-block> "
            "<block-count> <car-slot> [panel-id]\\n", argv[0]);
        return 1;
    }
    const std::filesystem::path input = argv[2];
    const std::filesystem::path output = argv[3];
    std::uint32_t first_block = 0, block_count = 0, car_slot = 0, panel_id = 0;
    if (!parse_u32(argv[4], first_block) || !parse_u32(argv[5], block_count) ||
        !parse_u32(argv[6], car_slot) ||
        (argc == 8 && !parse_u32(argv[7], panel_id)) || car_slot > 127 ||
        panel_id > 15 || block_count == 0) {
        std::fprintf(stderr, "invalid fixture range, car slot, or panel id\\n");
        return 1;
    }
    std::error_code error;
    if (std::filesystem::equivalent(input, output, error) && !error) {
        std::fprintf(stderr, "refusing to overwrite the source replay\\n");
        return 1;
    }
    const auto bytes = read_all(input.string());
    if (bytes.empty()) {
        std::fprintf(stderr, "couldn't read %s\\n", input.string().c_str());
        return 1;
    }
    opennr::RpyReplay replay;
    try {
        replay = opennr::RpyReplay::parse(bytes);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "parse error: %s\\n", e.what());
        return 1;
    }
    if (first_block >= replay.frame_blocks.size() ||
        block_count > replay.frame_blocks.size() - first_block) {
        std::fprintf(stderr, "fixture range exceeds %zu replay blocks\\n",
                     replay.frame_blocks.size());
        return 1;
    }
    std::vector<opennr::RpyType11FixtureEntry> entries;
    entries.reserve(block_count);
    for (std::uint32_t block = first_block; block < first_block + block_count; ++block) {
        const auto samples = replay.samples_at_block(block);
        const auto source = std::find_if(samples.cars.begin(), samples.cars.end(),
            [car_slot](const opennr::RpyCarSample& car) {
                return car.car_index == car_slot;
            });
        if (source == samples.cars.end()) continue;
        opennr::RpyRptpType11DebrisState state;
        state.pool_slot = 0;
        state.generation_id = 1;
        state.height_code = source->vertical_code;
        state.kind = 2;
        state.lateral_code = source->lateral_code;
        state.along_track_code = source->along_track_code;
        state.appearance = std::uint16_t(car_slot | (panel_id << 7));
        entries.push_back({block, state});
    }
    if (entries.empty()) {
        std::fprintf(stderr, "no type-3 sample for car slot %u in the selected range\\n",
                     car_slot);
        return 1;
    }
    std::vector<std::uint8_t> fixture;
    try {
        fixture = opennr::insert_rpy_type11_fixture_events(bytes, entries);
        const auto verified = opennr::RpyReplay::parse(fixture);
        if (verified.rptp_type11_debris.size() != entries.size())
            throw std::runtime_error("fixture type-11 count does not round-trip");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fixture error: %s\\n", e.what());
        return 1;
    }
    if (!write_all_new_file(output, fixture)) {
        std::fprintf(stderr, "couldn't write %s\\n", output.string().c_str());
        return 1;
    }
    std::printf("Wrote type-11 follow fixture %s (%zu records; source unchanged)\\n",
                output.string().c_str(), entries.size());
    return 0;
}

void dump(const opennr::RpyReplay& r) {
    std::printf("RPLY v%u\n", r.outer_version);
    std::printf("RPHD: frame_count=%u  rptp_body_size=%u  event_count=%u\n",
        r.header.frame_count, r.header.rptp_body_size, r.header.event_count);
    std::printf("Player: '%s %s'\n",
        r.header.player_first.c_str(), r.header.player_last.c_str());
    std::printf("WKNF: track='%s' class='%s'\n",
        r.weekend.track_name.c_str(), r.weekend.class_name.c_str());
    static constexpr const char* kSessionNames[] = {
        "Practice", "Qualifying", "Warmup", "Race"
    };
    std::printf("Weekend sessions:\n");
    for (std::size_t i = 0; i < r.weekend.sessions.size(); ++i) {
        const auto& s = r.weekend.sessions[i];
        std::printf("  %-11s flags=%02x lap_limit=%d duration=%.3f type=%u enabled=%s\n",
            kSessionNames[i], unsigned(s.flags_lo), int(s.lap_limit),
            double(s.duration_seconds), unsigned(s.type_tag),
            s.enabled_for_auto_advance() ? "yes" : "no");
    }
    std::printf("Drivers (%zu):\n", r.drivers.size());
    for (std::size_t i = 0; i < r.drivers.size() && i < 50; ++i) {
        const auto& d = r.drivers[i];
        std::printf("  [%2zu] #%-3u  %-12s %-16s  pos=%2u  skill=%u  car=%s\n",
            i, d.car_number,
            d.first_name.c_str(), d.last_name.c_str(),
            d.race_pos, d.skill_or_xp, d.car_file.c_str());
    }
    std::printf("Lap tables:\n");
    for (std::size_t t = 0; t < r.lap_tables.size(); ++t) {
        std::printf("  table[%zu]: %zu rows\n", t, r.lap_tables[t].rows.size());
    }
    std::printf("RPTP: %zu bytes, %u frame markers, frame range [%u..%u]\n",
        r.frame_data.size(), r.marker_count,
        r.first_frame_index, r.last_frame_index);
}

void dump_type14(const opennr::RpyReplay& r) {
    std::printf("type14 extensions: %zu marker(s), %u entries\n",
        r.marker_extensions.size(),
        r.rptp_counts.type14_extension_entries);
    for (const auto& marker : r.marker_extensions) {
        std::printf("  frame=%u block=%u entries=%zu\n",
            marker.frame_index, marker.frame_block, marker.entries.size());
        for (const auto& e : marker.entries) {
            std::printf(
                "    id=%02x car=%u wheel=%u intensity=%x raw_code=%04x "
                "pos=(%.6f,%.6f,%.6f) "
                "reserved=%08x tail_frame=%u packed=",
                unsigned(e.object_id), unsigned(e.car_slot()),
                unsigned(e.wheel_index()),
                unsigned(e.start_intensity_nibble()), unsigned(e.flags),
                double(e.x_fixed) / 128.0,
                double(e.y_fixed) / 128.0,
                double(e.z_fixed) / 128.0,
                e.reserved, e.frame_index);
            for (const auto byte : e.packed) {
                std::printf("%02x", unsigned(byte));
            }
            std::printf(" vertices=");
            for (const auto& point : e.auxiliary_vertices()) {
                std::printf("(%d,%d,%d;c=%x)",
                    int(point.x_offset_code), int(point.y_offset_code),
                    int(point.z_offset_code),
                    unsigned(point.intensity_nibble));
            }
            std::printf("\n");
        }
    }
}

void dump_type11(const opennr::RpyReplay& r) {
    std::printf("type11 debris states: %zu\n", r.rptp_type11_debris.size());
    for (const auto& d : r.rptp_type11_debris) {
        const auto angles = d.orientation_rad();
        std::printf(
            "  block=%u body=%u slot=%u generation=%u kind=%u car=%u panel=%u paint=%u "
            "track=(%.6f,%.6f) height=%.6f angles=(%.6f,%.6f,%.6f) "
            "codes=(%u,%d,%u)\n",
            d.frame_block, d.body_offset, unsigned(d.pool_slot), d.generation_id,
            unsigned(d.kind), unsigned(d.car_slot()), unsigned(d.panel_id()),
            unsigned(d.paint_id()), double(d.along_track()), double(d.lateral()),
            double(d.height_above_ground()), double(angles[0]), double(angles[1]),
            double(angles[2]), unsigned(d.along_track_code), d.lateral_code,
            unsigned(d.height_code));
    }
}

void dump_event_counts(const opennr::RpyReplay& r) {
    std::array<std::uint32_t, 65536> counts{};
    for (const auto& event : r.events) ++counts[event.type];
    std::printf("RPTP event counts:\n");
    for (std::size_t type = 0; type < counts.size(); ++type) {
        if (counts[type] != 0) {
            std::printf("  type=%zu count=%u\n", type, counts[type]);
        }
    }
}

void dump_type4(const opennr::RpyReplay& r) {
    std::printf("type4 session checkpoints: %zu\n", r.rptp_type4_checkpoints.size());
    for (const auto& cp : r.rptp_type4_checkpoints) {
        std::printf("  body=%u tag=%02x state=%02x%02x%02x sim=%.6f session=%08x reserved=%08x\n",
            cp.body_offset, unsigned(cp.sub_tag), unsigned(cp.state_bytes[0]),
            unsigned(cp.state_bytes[1]), unsigned(cp.state_bytes[2]),
            double(cp.sim_time), cp.session_id, cp.reserved);
    }
}

void dump_editor(const opennr::RpyReplay& r) {
    std::printf("Replay-editor events (types 19..28):\n");
    std::size_t frame = 0;
    for (std::size_t index = 0; index < r.events.size(); ++index) {
        const auto& event = r.events[index];
        if (event.type < 19 || event.type > 28) continue;
        while (frame + 1 < r.frame_blocks.size() &&
               index >= r.frame_blocks[frame].first_event +
                            r.frame_blocks[frame].event_count) {
            ++frame;
        }
        std::printf("  event=%zu block=%zu type=%u logical=%u payload=",
                    index, frame, unsigned(event.type),
                    unsigned(event.logical_size));
        const auto payload_offset = std::size_t(event.body_offset) + 4u;
        const auto payload_size = std::size_t(event.logical_size) - 4u;
        for (std::size_t byte = 0; byte < payload_size; ++byte)
            std::printf("%02x", unsigned(r.frame_data[payload_offset + byte]));
        std::printf("\n");
    }
}

int validate(const opennr::RpyReplay& r) {
    int errors = 0;
    auto err = [&](const char* fmt, auto... args) {
        std::fprintf(stderr, "[validate] ");
        std::fprintf(stderr, fmt, args...);
        std::fprintf(stderr, "\n");
        ++errors;
    };
    auto warn = [&](const char* fmt, auto... args) {
        std::fprintf(stderr, "[validate WARN] ");
        std::fprintf(stderr, fmt, args...);
        std::fprintf(stderr, "\n");
    };

    // 1) RPHD frame_count and the scanned marker_count should agree to
    // within ~1%. They aren't strictly identical because some frames
    // emit events instead of (or in addition to) a plain frame-tick
    // marker, and the final shutdown frame may not emit one. test.rpy
    // shows a 0.04% delta (28836 vs 28825).
    if (r.header.frame_count == 0) {
        err("RPHD frame_count == 0");
    } else {
        long long diff = static_cast<long long>(r.header.frame_count)
                       - static_cast<long long>(r.marker_count);
        if (diff < 0) diff = -diff;
        long long tolerance =
            std::max(1LL, static_cast<long long>(r.header.frame_count) / 100);
        if (diff > tolerance) {
            err("RPHD frame_count=%u vs marker_count=%u: delta %lld > tolerance %lld",
                r.header.frame_count, r.marker_count, diff, tolerance);
        } else if (diff > 0) {
            warn("frame_count/marker_count differ by %lld (within tolerance)", diff);
        }
    }

    // 2) RPHD rptp_body_size should equal the actual RPTP chunk byte length.
    if (r.header.rptp_body_size != r.frame_data.size()) {
        err("RPHD rptp_body_size=%u != actual RPTP %zu",
            r.header.rptp_body_size, r.frame_data.size());
    }

    // 3) LPTB lap-time monotonicity.
    int row_idx = 0;
    for (std::size_t t = 0; t < r.lap_tables.size(); ++t) {
        for (const auto& row : r.lap_tables[t].rows) {
            ++row_idx;
            std::uint32_t last = 0;
            for (std::size_t lap = 0; lap < row.lap_times_ms.size(); ++lap) {
                std::uint32_t ms = row.lap_times_ms[lap];
                // 0xFFFFFFFF or any value too small to be a NASCAR lap (< 12s)
                // is a sentinel for "lap not yet completed".
                if (ms == 0xFFFFFFFFu || ms < 12000u) continue;
                if (ms <= last) {
                    err("table[%zu].row[%d] car=%u: lap %zu time %u <= prev %u",
                        t, row_idx, row.car_index, lap, ms, last);
                    break;
                }
                last = ms;
            }
        }
    }

    // 4) DRNT car-number plausibility — informational only. Real NASCAR
    // numbers are 0..199 historically (Jack Sprague raced #0, and
    // cars-with-leading-zeros like #01 appear in stock data encoded as
    // u32 values up to ~2200, so the field isn't a clean integer).
    for (std::size_t i = 0; i < r.drivers.size(); ++i) {
        std::uint32_t cn = r.drivers[i].car_number;
        if (i < 2) continue;   // Player + Pace Car slots
        if (cn > 0x4000) {
            warn("driver[%zu] car_number=%u is large; check encoding", i, cn);
        }
    }

    // 5) Player name should be non-empty for the player slot (drivers[0]).
    if (!r.drivers.empty()) {
        if (r.drivers[0].first_name.empty() && r.drivers[0].last_name.empty()
            && r.header.player_first.empty() && r.header.player_last.empty()) {
            warn("player slot has no name");
        }
    }

    if (errors == 0) {
        std::printf("validation: OK\n");
    } else {
        std::printf("validation: %d error(s)\n", errors);
    }
    return errors == 0 ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--make-type11-follow") == 0)
        return make_type11_follow_fixture(argc, argv);
    bool do_validate = false;
    bool do_dump_type14 = false;
    bool do_dump_type11 = false;
    bool do_dump_type4 = false;
    bool do_event_counts = false;
    bool do_dump_editor = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--validate") == 0) {
            do_validate = true;
        } else if (std::strcmp(argv[i], "--dump-type14") == 0) {
            do_dump_type14 = true;
        } else if (std::strcmp(argv[i], "--dump-type11") == 0) {
            do_dump_type11 = true;
        } else if (std::strcmp(argv[i], "--dump-type4") == 0) {
            do_dump_type4 = true;
        } else if (std::strcmp(argv[i], "--event-counts") == 0) {
            do_event_counts = true;
        } else if (std::strcmp(argv[i], "--dump-editor") == 0) {
            do_dump_editor = true;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        std::fprintf(stderr,
            "usage: %s [--validate] [--dump-type11] [--dump-type14] [--event-counts] "
            "[--dump-type4] [--dump-editor] "
            "<replay.rpy>\n"
            "       %s --make-type11-follow <in.rpy> <out.rpy> <first-block> "
            "<block-count> <car-slot> [panel-id]\n",
            argv[0], argv[0]);
        return 1;
    }
    auto data = read_all(path);
    if (data.empty()) {
        std::fprintf(stderr, "couldn't read %s\n", path);
        return 1;
    }
    opennr::RpyReplay r;
    try {
        r = opennr::RpyReplay::parse(data);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "parse error: %s\n", e.what());
        return 1;
    }
    dump(r);
    if (do_dump_type11) dump_type11(r);
    if (do_dump_type4) dump_type4(r);
    if (do_dump_type14) dump_type14(r);
    if (do_event_counts) dump_event_counts(r);
    if (do_dump_editor) dump_editor(r);
    if (do_validate) return validate(r);
    return 0;
}
