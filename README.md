# Open NR2003 format tools

This repository contains standalone tools for Papyrus file formats.
The build does not read files from another source checkout.

Included formats are DAT, CAR, SIM, ACD, 3DO, PTF, LYT, RPY, BFF, and STP.
The repository contains no simulation runner.

The Windows package includes a track viewer for PTF track geometry.
The viewer has solid, wireframe, and collision-wireframe layers.
You can display any combination of these layers.
The solid view displays referenced MIP textures, walls, and placed `.3do` objects.

Read the [file format index](docs/formats/README.md) for format details and tool names.

## Requirements

- CMake 3.20 or a later version
- A C++20 compiler

The build has no SDL, FFmpeg, Vulkan, or D3D dependency.

## Build

Run these commands from the repository root:

```text
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix package
```

The install command puts programs in `package/bin`.
It puts the format references in `package/docs`.

## Track viewer

Run `opennr_track_viewer` on Windows.
Then open a track folder or an extracted `.ptf` file.

Use `opennr_track_viewer --load track-folder` to check a track without a window.
The command writes a log that lists missing 3DO objects and their failure reasons.

A track folder can contain a loose PTF file or a DAT archive.
The viewer reads the PTF entry from the DAT archive when necessary.
It reads resources from the track DAT before it reads loose files in the track folder.
Use **File > Select Shared Folder** to add `shared.dat` and loose shared resources.
The viewer does not search subfolders for loose resources.

Use these keys to toggle the view layers:

- `1`: Toggle the solid layer
- `2`: Toggle the wireframe layer
- `3`: Toggle the collision-wireframe layer
- `F` or `R`: Reset the camera

Use the left mouse button to orbit the camera.
Use the right mouse button to move the camera target.
Use the mouse wheel to change the camera distance.

Read the [track viewer guide](docs/track-viewer.md) for all inputs, colors, logs, and command-line options.

## Automated builds and releases

GitHub Actions builds Windows and Linux packages for pull requests and updates to `main`.
Push a version tag to publish a GitHub release.
Tags can use `0.1.0` or `v0.1.0` form.

```text
git tag v0.1.0
git push origin v0.1.0
```

The release contains a ZIP package for Windows and a compressed TAR package for Linux.

To publish an existing tag, run the `Build and release` workflow manually.
Set `release_tag` to the complete tag, such as `0.1.0`.

The manual workflow gets source files from that tag before it builds the packages.
An empty `release_tag` builds packages without publishing a release.

## Licenses

Project code uses the MIT License in [LICENSE](LICENSE).
The PKWARE library keeps its license in `third_party/pklib/LICENSE`.
