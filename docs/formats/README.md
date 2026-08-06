# File format reference

This directory documents the binary formats that the included tools can read.

| Format | Reference | Tool |
| --- | --- | --- |
| `.dat` | [DAT archive](dat.md) | `opennr_dat_tool` |
| `.car` | [CAR container](car.md) | `opennr_car_tool` |
| `.sim`, `.acd` | [PGTS container](sim-acd.md) | `opennr_sim_tool` |
| `.3do`, `.ptf` | [Typed object stream](3do-ptf.md) and [descriptor bodies](3do-ptf-descriptors.md) | `opennr_object_tool`, `opennr_track_viewer` |
| `.lyt` | [Layout](lyt.md) | `opennr_lyt_dump` |
| `.rpy` | [Replay](rpy.md) | `opennr_rpy_tool` |
| `.bff` | [Bitmap font](bff.md) | `opennr_bff_inspect`, `opennr_bff_render_text` |
| `.stp` | [Stamp image](stp.md) | `opennr_stp_dump` |

All integer fields use little-endian byte order unless a reference states a different order.
Sizes in chunk headers exclude the 12-byte chunk header.
