# SIM and ACD file format

`.sim` and `.acd` files use the PGTS chunked container.
The parser preserves undecoded bytes for a lossless write operation.

## FourCC byte order

PGTS tags use display order on disk.
The bytes for `PGTS` are `P G T S`.

## Chunk header

Every chunk uses this 12-byte header:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 4 | FourCC | Tag in display order |
| `0x04` | 4 | `u32` | Chunk version |
| `0x08` | 4 | `u32` | Body size |

The outer `PGTS` body contains `HGTS`, `DGTS`, and `TGTS` chunks.
Two `0x20` bytes can appear before `DGTS`.

Common versions are `PGTS=5`, `HGTS=3`, `DGTS=13`, and `TGTS=1`.

## HGTS body

`HGTS` contains metadata and two copies of an embedded filename.
The first name follows a `0x2a` delimiter near the start.
The second name starts at body offset `0x102`.

Each name is zero-terminated ASCII.
Unused bytes remain part of the fixed metadata record.

## DGTS body

Version 13 uses a 272-byte body.
The first `0x90` bytes contain nine arrays with four values each.
Array order is left-front, right-front, left-rear, and right-rear.

| Offset | Type | Field |
| ---: | --- | --- |
| `0x000` | `f32[4]` | Tire pressure |
| `0x010` | `i32[4]` | Low-speed bump |
| `0x020` | `i32[4]` | High-speed bump |
| `0x030` | `i32[4]` | Low-speed rebound |
| `0x040` | `i32[4]` | High-speed rebound |
| `0x050` | `f32[4]` | Spring rate |
| `0x060` | `f32[4]` | Spring-rubber values |
| `0x070` | `f32[4]` | Spring transition width |
| `0x080` | `f32[4]` | Camber |

The remaining known fields are listed below:

| Offset | Type | Field |
| ---: | --- | --- |
| `0x090` | `f32` | Left-front ride-height target |
| `0x094` | `f32` | Left-rear ride-height target |
| `0x098` | `f32` | Right-rear ride-height target |
| `0x09c` | `f32` | Brake-bias fraction |
| `0x0a0` | `f32` | Front toe-out |
| `0x0a4` | `f32` | Rear toe-out |
| `0x0a8` | `i32` | Front anti-roll index |
| `0x0ac` | `i32` | Rear anti-roll index |
| `0x0b0` | `f32` | Left rear track-bar height |
| `0x0b4` | `f32` | Right rear track-bar height |
| `0x0b8` | `i32` | Unknown flag |
| `0x0bc` | `f32` | Steering ratio |
| `0x0c0` | `f32` | Fuel mass in kilograms |
| `0x0c4` | 4 bytes | Reserved setup slot |
| `0x0c8` | `i32` | Gear count |
| `0x0cc` | `i32` | Reverse-gear index |
| `0x0d0` | `i32[4]` | Forward-gear indices |
| `0x0e0` | `i32` | Final-drive index |
| `0x0e4` | 4 bytes | Reserved setup slot |
| `0x0e8` | `f32` | Grille-tape fraction |
| `0x0ec` | `f32` | Unknown mirror value |
| `0x0f0` | `f32` | Rear-spoiler angle minus 45 degrees |
| `0x0f4` | `f32` | Left-to-right weight bias |
| `0x0f8` | `f32` | Front-to-rear weight bias |
| `0x0fc` | `f32` | Cross-weight value |
| `0x100` | `f32` | Front stagger radius delta |
| `0x104` | `f32` | Rear stagger radius delta |
| `0x108` | `f32` | Left-front caster |
| `0x10c` | `f32` | Right-front caster |

The right-front ride-height target is not stored in version 13.

Version 13 stores four forward-gear indices at `0x0d0` through `0x0dc`.
Fifth-gear and sixth-gear indices are not part of this body.

## TGTS body

`TGTS` contains zero-terminated ASCII text.
Version 1 commonly uses a fixed 2048-byte body.

## Inspect a file

Run the format-only inspector:

```text
opennr_sim_tool file.sim
```
