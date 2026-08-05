// dat_tool: list or extract a Papyrus .dat archive.
//
// Usage:
//   opennr_dat_tool                         (native archive browser)
//   opennr_dat_tool gui [archive.dat]
//   opennr_dat_tool list <archive.dat>
//   opennr_dat_tool extract <archive.dat> <out_dir>
//   opennr_dat_tool extract-one <archive.dat> <entry-name> <out-file>

#include "fs/dat_archive.h"

#ifdef _WIN32
#include "dat_tool_gui.h"
#endif

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>

namespace fs = std::filesystem;

static int do_list(const fs::path& archive) {
    auto arc = opennr::DatArchive::load(archive);
    std::printf("%s: %zu entries, header_hash=0x%08x\n",
                archive.filename().string().c_str(),
                arc.entries().size(),
                arc.header_hash());
    for (const auto& e : arc.entries()) {
        const char* tag = e.is_compressed() ? "COMP" : "STOR";
        std::printf("  [%s] off=0x%08x usz=%10u csz=%10u  %s\n",
                    tag, e.data_offset, e.uncompressed_size, e.compressed_size,
                    e.name.c_str());
    }
    return 0;
}

static int do_extract(const fs::path& archive, const fs::path& out_dir) {
    auto arc = opennr::DatArchive::load(archive);
    fs::create_directories(out_dir);

    std::size_t stored = 0, decompressed = 0, failed = 0;
    for (const auto& e : arc.entries()) {
        // Normalize the entry name's path separators.  Papyrus uses
        // backslashes inside archive entry names.
        std::string sanitized = e.name;
        for (char& c : sanitized) {
            if (c == '\\') c = '/';
        }
        fs::path target = out_dir / sanitized;
        fs::create_directories(target.parent_path());

        std::vector<std::uint8_t> bytes;
        try {
            bytes = arc.read(e);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "  warn: %s: %s\n", e.name.c_str(), ex.what());
            ++failed;
            continue;
        }
        std::ofstream out(target, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "  warn: cannot write %s\n", target.string().c_str());
            ++failed;
            continue;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (e.is_compressed()) {
            ++decompressed;
        } else {
            ++stored;
        }
    }
    std::printf("Wrote %zu stored + %zu decompressed entries (%zu failed) to %s\n",
                stored, decompressed, failed, out_dir.string().c_str());
    return failed == 0 ? 0 : 1;
}

// Read one named archive entry without materialising the rest of the DAT.
// This is useful for inspecting a single compressed game asset (for example a
// .3do) and keeps callers from having to extract an entire series archive.
static int do_extract_one(const fs::path& archive, const std::string& name,
                          const fs::path& out_file) {
    const auto arc = opennr::DatArchive::load(archive);
    const auto it = std::find_if(arc.entries().begin(), arc.entries().end(),
                                 [&](const opennr::DatEntry& e) {
                                     return e.name == name;
                                 });
    if (it == arc.entries().end()) {
        std::fprintf(stderr, "entry not found: %s\n", name.c_str());
        return 1;
    }
    const auto bytes = arc.read(*it);
    if (out_file.has_parent_path()) fs::create_directories(out_file.parent_path());
    std::ofstream out(out_file, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", out_file.string().c_str());
        return 1;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        std::fprintf(stderr, "failed to write %s\n", out_file.string().c_str());
        return 1;
    }
    std::printf("Wrote %s (%zu bytes) to %s\n", name.c_str(), bytes.size(),
                out_file.string().c_str());
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    if (argc == 1) {
        return run_dat_tool_gui();
    }
    if (std::string(argv[1]) == "gui") {
        return run_dat_tool_gui(argc >= 3 ? fs::path(argv[2]) : fs::path{});
    }
#endif
    if (argc < 3) {
        std::fprintf(stderr,
#ifdef _WIN32
                     "usage: %s                         (open the archive browser)\n"
                     "       %s gui [archive.dat]\n"
                     "       %s list <archive.dat>\n"
                     "       %s extract <archive.dat> <out_dir>\n"
                     "       %s extract-one <archive.dat> <entry-name> <out-file>\n",
                     argv[0], argv[0], argv[0], argv[0], argv[0]);
#else
                     "usage: %s list <archive.dat>\n"
                     "       %s extract <archive.dat> <out_dir>\n"
                     "       %s extract-one <archive.dat> <entry-name> <out-file>\n",
                     argv[0], argv[0], argv[0]);
#endif
        return 2;
    }
    std::string cmd = argv[1];
    try {
        if (cmd == "list") {
            return do_list(argv[2]);
        }
        if (cmd == "extract") {
            if (argc < 4) {
                std::fprintf(stderr, "extract requires <out_dir>\n");
                return 2;
            }
            return do_extract(argv[2], argv[3]);
        }
        if (cmd == "extract-one") {
            if (argc < 5) {
                std::fprintf(stderr, "extract-one requires <entry-name> <out-file>\n");
                return 2;
            }
            return do_extract_one(argv[2], argv[3], argv[4]);
        }
        std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
        return 2;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
