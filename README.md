# OpenNR format tools

This repository contains standalone tools for Papyrus file formats.
The build does not read files from another source checkout.

Included formats are DAT, CAR, SIM, ACD, 3DO, PTF, LYT, RPY, BFF, and STP.
The repository contains no simulation runner or renderer component.

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

## Automated builds and releases

GitHub Actions builds Windows and Linux packages for pull requests and updates to `main`.
Push a tag that starts with `v` to publish a GitHub release.

```text
git tag v0.1.0
git push origin v0.1.0
```

The release contains a ZIP package for Windows and a compressed TAR package for Linux.

## Licenses

Project code uses the MIT License in [LICENSE](LICENSE).
The PKWARE library keeps its license in `third_party/pklib/LICENSE`.
