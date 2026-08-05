# RPY replay format

An `.rpy` file is a chunked replay container.
Its main data chunk contains frames and typed events.

## Chunk header

Tags use reversed byte order on disk.
For example, logical tag `RPLY` appears as `Y L P R`.

Every chunk uses this 12-byte header:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 4 | FourCC | Reversed tag bytes |
| `0x04` | 4 | `u32` | Version |
| `0x08` | 4 | `u32` | Body size |

Sibling chunks can have up to three zero or `0x20` padding bytes.

## Top-level chunks

The outer chunk is `RPLY`, commonly with version 5.
Its body contains these chunks:

| Chunk | Purpose |
| --- | --- |
| `RPHD` | Replay header |
| `WKNF` | Session metadata |
| `DRLS` | Driver-list container |
| `LPTB` | Lap-table container |
| `RPTP` | Frame and event stream |
| `RPRS` | Zero-size end marker |

## RPHD body

The `RPHD` body has 60 bytes:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 4 | `u32` | Frame count |
| `0x04` | 4 | `u32` | RPTP body size |
| `0x08` | 4 | `u32` | Event count |
| `0x0c` | 4 | `u32` | Reserved |
| `0x10` | 8 | ASCII | First name |
| `0x18` | 8 | bytes | Reserved |
| `0x20` | 8 | ASCII | Last name |
| `0x28` | 16 | bytes | Reserved |
| `0x38` | 4 | `u32` | Trailing version or flag |

Some lesson replays extend this body after `0x3c`.
The extension contains a zero-terminated UTF-16LE summary.
Ordinary replays normally have no summary extension.

## WKNF body

The `WKNF` body has 173 bytes.
It contains two 32-byte names and four session records.

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 32 | ASCII | Track name |
| `0x20` | 32 | ASCII | Class name |
| `0x40` | 4 | `u32` | Session identifier |
| `0x44` | 24 | bytes | Timestamp and reserved counters |
| `0x5c` | 1 | `u8` | Temperature value |
| `0x5d` | 3 | bytes | Reserved |
| `0x60` | 4 | `u32` | Length percentage |
| `0x64` | 2 | bytes | Reserved |
| `0x66` | 1 | `u8` | Participant count field |
| `0x67` | 5 | bytes | Reserved |
| `0x6c` | 32 | records | Four session records |
| `0x8c` | 33 | bytes | Reserved tail |

Each session record has eight bytes:

| Offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u8` | Flags |
| `0x01` | `i16` | Lap limit |
| `0x03` | `f32` | Duration in seconds |
| `0x07` | `u8` | Type tag |

The duration field is not aligned to four bytes.

## Driver and lap records

`DRLS` contains `DRNT` records.
Each `DRNT` body has 175 bytes.

Each `DRNT` record has this complete offset map:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 16 | ASCII | First name |
| `0x10` | 16 | ASCII | Last name |
| `0x20` | 80 | bytes | Reserved |
| `0x70` | 32 | ASCII | Car filename |
| `0x90` | 4 | `u32` | Manufacturer index |
| `0x94` | 4 | `u32` | Driver flags |
| `0x98` | 4 | `u32` | Reserved |
| `0x9c` | 4 | `u32` | Car number |
| `0xa0` | 4 | `u32` | Skill or experience value |
| `0xa4` | 4 | `u32` | List identifier |
| `0xa8` | 4 | `u32` | Starting position |
| `0xac` | 1 | `u8` | AI-control flag |
| `0xad` | 1 | `u8` | Racer flag |
| `0xae` | 1 | `u8` | Player flags |

`LPTB` contains `LPRO` records.
Each `LPRO` body has 76 bytes.

An `LPRO` record has this structure:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 4 | `u32` | Driver-list index |
| `0x04` | 4 | `u32` | Packed status and time value |
| `0x08` | 72 | `u32[18]` | Cumulative lap timestamps in milliseconds |

Bit 30 of the packed word is a status flag.
The low 30 bits contain a time-like value.
Bit 31 is reserved in known files.

## RPTP frames

The `RPTP` body contains consecutive frame blocks.
It has no chunk preamble.

Each frame starts with this eight-byte header:

| Offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u16` | Total frame size |
| `0x02` | `u16` | Previous frame size |
| `0x04` | `u16` | Sequence value |
| `0x06` | `u16` | Flags |

The total size includes the frame header.
Each frame after the first repeats the preceding frame size.

## RPTP events

Events start at frame offset `0x08`.
Each event starts with a four-byte header:

| Offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u16` | Event type |
| `0x02` | `u16` | Logical event size |

The physical event stride is `align4(logical_size)`.
Aligned events must consume the frame exactly.
Unknown event types remain valid opaque data.

Known event families include car samples, checkpoints, debris states, markers, and editor commands.
Editor command types occupy values 19 through 28.

## Event type 3: car samples

A type 3 event has logical size `4 + 16 * sample_count`.
Each sample has four packed little-endian words.

| Bits | Meaning |
| --- | --- |
| `w0[0]` | Status bit |
| `w0[10:1]` | Task tick modulo 1024 |
| `w0[15:11]` | Five-bit throttle code |
| `w1[9:0]` | Signed ten-bit pitch code |
| `w1[15:10]` | Signed six-bit steering code |
| `d1[23:0]` | Unsigned along-track coordinate |
| `d1[30:24]` | Car index from 0 through 47 |
| `d1[31]` | Discontinuity or status flag |
| `d2[9:0]` | Signed ten-bit roll code |
| `d2[19:10]` | Unsigned ten-bit RPM code |
| `d2[20]` | State flag |
| `d2[29:21]` | Unsigned nine-bit vertical code |
| `d2[31:30]` | Two-bit mode |
| `d3[11:0]` | Signed 12-bit yaw code |
| `d3[28:12]` | Signed 17-bit lateral code |
| `d3[31:29]` | Three status flags |

The sample count comes from the event size.
It does not come from the participant count in `WKNF`.

## Event type 4: session checkpoint

Type 4 has a logical size of 20 bytes.
Its 16-byte payload has this structure:

| Payload offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u8` | Subtag |
| `0x01` | `u8[3]` | Packed state bytes |
| `0x04` | `f32` | Simulation time in seconds |
| `0x08` | `u32` | Session identifier |
| `0x0c` | `u32` | Reserved |

The session identifier must agree with the value in `WKNF`.

## Event type 9: state value

Type 9 has a logical size of 14 bytes and a physical stride of 16 bytes.
Its ten-byte payload has this structure:

| Payload offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u32` | State code |
| `0x04` | `u16` | Reserved |
| `0x06` | `f32` | Unaligned scale value |

Known state codes are 1, 2, and 5.
Tools must preserve other state codes.

## Event type 11: packed object state

Type 11 has a logical size of 20 bytes.
Its payload contains four packed words.

| Storage | Bits | Meaning |
| --- | --- | --- |
| Dword 0 | 0 through 4 | Object-pool slot |
| Dword 0 | 5 through 22 | Generation identifier |
| Dword 0 | 23 through 31 | Height code |
| Dword 1 | 0 through 1 | Object kind |
| Dword 1 | 2 through 18 | Signed lateral code |
| Dword 2 | 0 through 23 | Along-track code |
| Bytes 11 through 13 | all | Z, Y, and X orientation codes |
| Word at byte 14 | 0 through 6 | Source car slot |
| Word at byte 14 | 7 through 10 | Panel or model identifier |
| Word at byte 14 | 11 through 13 | Appearance identifier |
| Word at byte 14 | 14 through 15 | Reserved |

The along-track code uses a scale of `0.0005` metres per unit.

## Event type 13: recorder counter

Type 13 has a logical size of eight bytes.
Its payload contains one `u32` cumulative counter.

The counter unit is unspecified.
Editors must retain the exact value.

## Event type 14: frame marker

A basic type 14 event has a logical size of eight bytes.
Its payload contains one `u32` frame index.

An extended marker has logical size `8 + 35 * entry_count`.
Each extension entry describes one four-point ribbon mark.

| Entry offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `i32` | Base X in units of 1/128 metre |
| `0x04` | `i32` | Base Y in units of 1/128 metre |
| `0x08` | `i32` | Base Z in units of 1/128 metre |
| `0x0c` | `u16` | Base intensity code |
| `0x0e` | `u32[3]` | Three packed auxiliary points |
| `0x1a` | `u8` | Object identifier |
| `0x1b` | `u32` | Reserved |
| `0x1f` | `u32` | Repeated frame index |

Each auxiliary point uses this packed word:

| Bits | Meaning |
| --- | --- |
| 0 through 9 | Signed X offset in units of 1/128 metre |
| 10 through 19 | Signed Y offset in units of 1/128 metre |
| 20 through 27 | Signed Z offset in units of 1/128 metre |
| 28 through 31 | Intensity nibble |

The object identifier uses `car_index * 4 + wheel_index`.

## Event types 19 through 28

These events store replay-editing commands.
Their order in a frame is significant.

| Type | Operation | Payload in wire order |
| ---: | --- | --- |
| 19 | Image stamp | `f32` fade time, `f32` lifespan, `u16` X, `u16` Y, source byte, zero-terminated ASCII name |
| 20 | Sound | Source byte and zero-terminated ASCII name |
| 21 | Text | `f32` fade time, `f32` lifespan, three `u16` geometry values, enabled byte, zero-terminated UTF-16LE text |
| 22 | Fade | `f32` lifespan and current or previous fade-state bits |
| 23 | Camera | New camera byte and previous camera byte |
| 24 | Car target | New signed car byte and previous signed car byte |
| 25 | Playback rate | `f32` lifespan and packed new or previous rate codes |
| 26 | Marker | No payload |
| 27 | Toggle | Index byte, forward-value byte, and reverse-value byte |
| 28 | Volume | `f32` lifespan, `f32` target volume, and `f32` previous volume |

For type 24, a car value of `-1` means no selected car.

For type 21, enabled value zero selects literal UTF-16LE text.
Enabled value one selects a decimal resource identifier stored as text.

Resource events store filenames only.
They do not embed image or audio bytes.

## Editing rules

After an edit, recalculate every changed event size and frame size.
Then update the previous-frame size chain.

Update the RPTP body size in `RPHD`.
Also update the `RPTP` and outer `RPLY` chunk sizes.

The format has no replay checksum.

## Inspect a replay

Display a replay summary:

```text
opennr_rpy_tool file.rpy
```

Make sure that the structure is internally consistent:

```text
opennr_rpy_tool --validate file.rpy
```
