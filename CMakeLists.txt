cmake_minimum_required(VERSION 3.20)
project(OpenNRFormats VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENNR_FORMATS_STATIC_RUNTIME
       "Link the static Microsoft C and C++ runtimes"
       ON)

function(opennr_formats_use_runtime target)
    if(TARGET ${target} AND OPENNR_FORMATS_STATIC_RUNTIME AND
       CMAKE_CXX_COMPILER_ID MATCHES "MSVC|Clang")
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(opennr_formats STATIC
    src/core/byte_reader.cpp
    src/fs/dat_archive.cpp
    src/fs/dcl_blast.cpp
    src/fs/car_file.cpp
    src/fs/sim_file.cpp
    src/fs/gear_ratio_table.cpp
    src/fs/descriptors.cpp
    src/fs/object_stream.cpp
    src/fs/object_tree.cpp
    src/fs/papyrus_archive.cpp
    src/fs/papyrus_piff.cpp
    src/fs/papyrus_descriptors.cpp
    src/fs/lyt_layout.cpp
    src/fs/rpy_editor.cpp
    src/fs/rpy_replay.cpp
    src/fs/bff_font.cpp
    src/fs/stp_image.cpp
    src/fs/mip_texture.cpp
    src/fs/tga_image.cpp
    src/render/image_loader.cpp
    src/viewer/track_view.cpp
    third_party/pklib/src/PKWareLUT/PKWareLUTs.c
    third_party/pklib/src/implode/implode.c)
set_source_files_properties(
    third_party/pklib/src/PKWareLUT/PKWareLUTs.c
    third_party/pklib/src/implode/implode.c
    PROPERTIES LANGUAGE CXX)
target_include_directories(opennr_formats PUBLIC
    src
    third_party/pklib/include)
opennr_formats_use_runtime(opennr_formats)

function(opennr_add_format_tool target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE opennr_formats)
    opennr_formats_use_runtime(${target})
    set_property(GLOBAL APPEND PROPERTY OPENNR_FORMAT_TOOL_TARGETS ${target})
endfunction()

opennr_add_format_tool(opennr_dat_tool tools/cpp/dat_tool.cpp)
if(WIN32)
    target_sources(opennr_dat_tool PRIVATE
        tools/cpp/dat_tool_gui.h
        tools/cpp/dat_tool_gui.cpp)
    target_link_libraries(opennr_dat_tool PRIVATE
        comctl32 ole32 shell32 uxtheme)
endif()

opennr_add_format_tool(opennr_car_tool tools/cpp/car_tool.cpp)
opennr_add_format_tool(opennr_sim_tool tools/cpp/sim_tool.cpp)
opennr_add_format_tool(opennr_object_tool tools/cpp/object_tool.cpp)
opennr_add_format_tool(opennr_lyt_dump tools/cpp/lyt_dump.cpp)
opennr_add_format_tool(opennr_rpy_tool tools/cpp/rpy_tool.cpp)
opennr_add_format_tool(opennr_bff_inspect tools/cpp/bff_inspect.cpp)
opennr_add_format_tool(opennr_bff_render_text tools/cpp/bff_render_text.cpp)
opennr_add_format_tool(opennr_stp_dump tools/cpp/stp_dump.cpp)

if(WIN32)
    add_executable(opennr_track_viewer WIN32 tools/cpp/track_viewer.cpp)
    target_link_libraries(opennr_track_viewer PRIVATE
        opennr_formats comdlg32 gdi32 ole32 shell32 user32)
else()
    add_executable(opennr_track_viewer tools/cpp/track_viewer.cpp)
    target_link_libraries(opennr_track_viewer PRIVATE opennr_formats)
endif()
opennr_formats_use_runtime(opennr_track_viewer)
set_property(GLOBAL APPEND PROPERTY OPENNR_FORMAT_TOOL_TARGETS
    opennr_track_viewer)

get_property(OPENNR_FORMAT_TOOL_TARGETS GLOBAL
    PROPERTY OPENNR_FORMAT_TOOL_TARGETS)
install(TARGETS ${OPENNR_FORMAT_TOOL_TARGETS}
    RUNTIME DESTINATION bin)
install(DIRECTORY docs/ DESTINATION docs)
install(FILES README.md LICENSE DESTINATION .)
install(FILES third_party/pklib/LICENSE DESTINATION licenses
    RENAME pklib-LICENSE)
