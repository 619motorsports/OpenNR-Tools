// sim_tool: parse a Papyrus .sim or .acd file and print stored fields.

#include "fs/sim_file.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    const auto size = static_cast<std::size_t>(file.tellg());
    file.seekg(0);
    std::vector<std::uint8_t> bytes(size);
    if (!file.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(size))) {
        throw std::runtime_error("short read from " + path.string());
    }
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <file.sim|file.acd>\n", argv[0]);
        return 2;
    }

    try {
        const fs::path path = argv[1];
        const auto bytes = read_file(path);
        const auto file = opennr::SimFile::parse(bytes);

        std::printf("File          : %s (%zu bytes)\n",
                    path.filename().string().c_str(), bytes.size());
        std::printf("PGTS v%u  HGTS v%u  DGTS v%u\n",
                    file.pgts_version, file.hgts_version, file.dgts_version);
        std::printf("Embedded name : %s\n", file.embedded_name.c_str());
        std::printf("Description   : %s\n",
                    file.description.empty() ? "(none)" : file.description.c_str());

        std::printf("\nCorner fields (LF, RF, LR, RR)\n");
        for (std::size_t i = 0; i < file.setup.corner.size(); ++i) {
            const auto& corner = file.setup.corner[i];
            static constexpr const char* names[] = {"LF", "RF", "LR", "RR"};
            std::printf(
                "  %s pressure=%g bump=%d/%d rebound=%d/%d "
                "spring=%g rubber=%g camber=%g\n",
                names[i], corner.tire_pressure, corner.bump_low,
                corner.bump_high, corner.rebound_low, corner.rebound_high,
                corner.spring_rate, corner.spring_rubber, corner.camber);
        }

        std::printf("\nSetup fields\n");
        std::printf("  brake_bias=%g front_toe=%g rear_toe=%g\n",
                    file.setup.brake_bias, file.setup.front_toe_out,
                    file.setup.rear_toe_out);
        std::printf("  anti_roll_front=%d anti_roll_rear=%d\n",
                    file.setup.front_bar_setting,
                    file.setup.rear_bar_setting);
        std::printf("  track_bar_left=%g track_bar_right=%g\n",
                    file.setup.rear_track_bar_lr,
                    file.setup.rear_track_bar_rr);
        std::printf("  steering_ratio=%g fuel_load=%g grille_tape=%g\n",
                    file.setup.steer_ratio, file.setup.fuel_load_kg,
                    file.setup.grille_tape);
        std::printf("  spoiler=%g front_stagger=%g rear_stagger=%g\n",
                    file.setup.rear_spoiler_angle_deg,
                    file.setup.front_stagger_radius_delta_m,
                    file.setup.rear_stagger_radius_delta_m);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
