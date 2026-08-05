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

## WKNF body

The `WKNF` body has 173 bytes.
It contains two 32-byte names and four session records.

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 32 | ASCII | Track name |
| `0x20` | 32 | ASCII | Class name |
| `0x40` | 4 | `u32` | Session identifier |
| `0x5c` | 1 | `u8` | Temperature value |
| `0x60` | 4 | `u32` | Length percentage |
| `0x66` | 1 | `u8` | Participant count field |
| `0x6c` | 32 | records | Four session records |

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

Known fields include two names, a car filename, a number, identifiers, and three trailing flags.

`LPTB` contains `LPRO` records.
Each `LPRO` body has 76 bytes.

An `LPRO` record contains a car index, one status word, and 18 cumulative millisecond values.

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
