# Track viewer

The track viewer displays the geometry in a PTF track file.
It does not run vehicle physics or other simulation systems.

## Start the viewer

Run this program on Windows:

```text
opennr_track_viewer
```

Use **File > Open Track Folder** to select a track directory.
You can also drag a track directory into the window.

The viewer first searches the directory for a loose `.ptf` file.
If no loose file exists, the viewer searches a `.dat` archive for the PTF entry.

Use **File > Open PTF File** to select an extracted `.ptf` file.
You can also drag the file into the window.

Use **File > Select Shared Folder** to add a shared resource folder.
The viewer reloads the open track after you select the folder.

Use **File > Clear Shared Folder** to remove the shared resource folder.

The viewer writes a load log after it opens a track.
The default file name is `opennr_track_viewer_missing.log`.
The viewer puts this file in the track folder.

## Check a track from the command line

Use `--load` to load and check a track without a viewer window:

```text
opennr_track_viewer --load track-folder
```

Use `--shared` to add a shared resource folder:

```text
opennr_track_viewer --load --shared shared-folder track-folder
```

Use `--log` to select the load log path:

```text
opennr_track_viewer --load --shared shared-folder --log load.log track-folder
```

The command prints the loaded and missing object counts.
The load log identifies the PTF source and each resource location.
It lists each missing 3DO name, its instance count, and the failure reason.
The log contains `None` when the viewer finds and renders every 3DO object.

The GUI and snapshot commands also write this log.
Use `--log` with either command to select a different path.

## Move the camera

- Hold the left mouse button and move the mouse to orbit the track.
- Hold the right mouse button and move the mouse to move the camera target.
- Turn the mouse wheel to change the camera distance.
- Press `F` or `R` to reset the camera.

## Select view layers

Press `1` to toggle the solid layer.
This view displays the MIP textures that the surface and wall descriptors reference.
It also displays placed `.3do` objects and the track centerline.

Press `2` to toggle the wireframe layer.
This view displays the surface mesh in cyan.
It displays the visual wall mesh in gray and placed object meshes in green.
It displays the centerline in yellow.

Press `3` to toggle the collision-wireframe layer.
This view displays the surface mesh in amber.
It displays the collision wall mesh in red and the centerline in cyan.

You can display multiple layers at the same time.
For example, enable the solid and collision layers to compare the collision geometry with the rendered track.

The viewer keeps at least one layer active.
The **View** menu shows a check mark beside each active layer.

## Geometry sources

The surface mesh uses each `X_SectionDescriptor` in each track segment.
The viewer samples straight and curved segments between adjacent cross-sections.

Each cross-section supplies the lateral points, elevations, and surface slopes.
The viewer adds material boundaries from the `F_SectionDescriptor` data.

The solid view uses the surface code for each material band.
The surface code supplies a fallback color when its MIP texture is not available.

The visual wall mesh uses each `W_SectionDescriptor` and its visual-face offset.
The collision wall mesh uses the collision half-thickness from the same descriptor.

The viewer calculates the bank height separately for each wall face.
Thus, the bottom and top edges follow the cross-section bank.

The height-offset mode can put a visual wall along the cross-section normal.
This mode banks walls, fences, and their top faces with the track surface.
The collision wall uses the same normal when this mode is active.
Thus, the collision and visual wall faces stay aligned on a banked surface.

Each W-section record supplies separate appearances for the two wall faces and the top face.
The viewer uses the texture coordinates from the appearance pair for each face.

The PTF root can contain placed `TSOReferenceDescriptor` objects.
Each reference supplies a `.3do` name and a six-value placement transform.

The viewer reads each referenced `.3do` file and its base MIP texture.
It applies group, state, level-of-detail, and transform nodes to the object geometry.
For a state switch, the viewer selects the first state that contains geometry.
This rule skips empty states without drawing multiple states at the same time.

The collision view is a geometry inspection view.
It does not calculate contacts or move a vehicle.

## External resources

A PTF file stores resource names but does not contain the referenced resource data.
The viewer uses this search order for each `.3do` or `.mip` reference:

1. The main DAT archive in the track folder.
2. Loose files directly in the track folder.
3. The main DAT archive in the selected shared folder.
4. Loose files directly in the selected shared folder.

The viewer does not scan subfolders.
It can open an exact relative path that a PTF or 3DO file names.
For example, `trackmat/grass.mip` opens only that named file.
The viewer does not use subfolder files for a base-name search.
If the selected shared folder contains `shared.dat`, the viewer uses that archive first.

Open the complete track folder to display all textures and placed objects.
An extracted PTF file can use resources from its main sibling DAT archive.
It can also use loose files beside the PTF.

The viewer keeps one decoded copy of each MIP texture and each `.3do` model.
It uses a reduced render size while you move the camera.

## Save a snapshot

The snapshot command works on Windows and Linux.
It writes an uncompressed 32-bit BMP file.

```text
opennr_track_viewer --snapshot track.bmp track-folder
```

Use `--shared` to add a shared resource folder:

```text
opennr_track_viewer --snapshot track.bmp --shared shared-folder track-folder
```

Select one or more layers with `--mode`:

```text
opennr_track_viewer --snapshot solid.bmp --mode solid track.ptf
opennr_track_viewer --snapshot mesh.bmp --mode wire track.ptf
opennr_track_viewer --snapshot collision.bmp --mode collision track.ptf
opennr_track_viewer --snapshot comparison.bmp --mode solid+collision track.ptf
opennr_track_viewer --snapshot all-layers.bmp --mode all track.ptf
```

Use a plus sign, comma, or vertical bar to separate layer names.

Use `--size WIDTHxHEIGHT` to select the image size.
Each dimension must be from 64 through 8192 pixels.

```text
opennr_track_viewer --snapshot track.bmp --size 1920x1080 track-folder
```

Put a path in quotation marks when the path contains spaces.
