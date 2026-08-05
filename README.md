# Open NR2003 format tools

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
