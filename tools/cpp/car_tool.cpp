// car_tool: parse a Papyrus .car file and print summary info.
//
// Usage: opennr_car_tool <path/to/file.car>

#include "fs/car_file.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.car>\n", argv[0]);
        return 2;
    }
    fs::path path = argv[1];
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.string().c_str());
        return 1;
    }
    auto size = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::uint8_t> bytes(size);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) {
        std::fprintf(stderr, "short read\n");
        return 1;
    }

    try {
        auto car = opennr::CarFile::parse(bytes);
        std::printf("File         : %s (%zu bytes)\n", path.filename().string().c_str(), size);
        std::printf("CTYP         : 0x%08x\n", car.ctyp);
        std::printf("Driver       : %s\n",     car.driver_full_name().c_str());
        std::printf("Team         : %s\n",     car.team_name().c_str());
        std::printf("Sponsor      : %s\n",     car.sponsor().c_str());
        std::printf("Number       : %s\n",     car.car_number().c_str());
        std::printf("car_make     : %d\n",     car.car_make());
        std::printf("car_class    : %d\n",     car.car_class());
        std::printf("CINI sections: %zu\n",    car.ini.size());
        for (const auto& [name, sec] : car.ini) {
            std::printf("  [%s]  (%zu keys)\n", name.c_str(), sec.size());
        }
        std::printf("CTEX bytes   : %zu\n", car.ctex_bytes.size());
        std::printf("CREW bytes   : %zu\n", car.crew_bytes.size());
        bool dump_sections = (argc >= 3 && std::string(argv[2]) == "--sections");
        if (dump_sections) {
            for (const auto& [name, sec] : car.ini) {
                std::printf("\n[%s]\n", name.c_str());
                for (const auto& [k, v] : sec) {
                    std::printf("  %s = %s\n", k.c_str(), v.c_str());
                }
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "parse error: %s\n", e.what());
        return 1;
    }
}
