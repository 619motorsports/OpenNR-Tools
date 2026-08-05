#pragma once

#include <filesystem>

// Runs the native archive browser. An empty initial_archive opens the browser
// in its welcome state; otherwise that archive is loaded after the window is
// created. This is only implemented on Windows.
int run_dat_tool_gui(const std::filesystem::path& initial_archive = {});

