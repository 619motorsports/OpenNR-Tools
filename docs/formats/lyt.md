# LYT layout format

A `.lyt` file contains fixed-size widget records.
The file does not store the widget count.

## File header

The file starts with this 16-byte header:

| Offset | Type | Field | Required value |
| ---: | --- | --- | --- |
| `0x00` | `u32` | Version | `8` |
| `0x04` | `u32` | Record size | `1137` (`0x471`) |
| `0x08` | `u32` | Reserved | `0` |
| `0x0c` | `u32` | Reserved | `0` |

Calculate the widget count as `(file_size - 16) / 1137`.
The body size must be an exact multiple of the record size.

## Widget record

Each record is a 1137-byte tagged union.
Unused fields are normally zero.

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x000` | 32 | ASCII | Name |
| `0x100` | 4 | `u32` | Widget type |
| `0x104` | 4 | `i32` | X position |
| `0x108` | 4 | `i32` | Y position |
| `0x10c` | 4 | `i32` | Width |
| `0x110` | 4 | `i32` | Height |
| `0x114` | 4 | `u32` | Group identifier |
| `0x180` | 1 | `u8` | Image-animation flag |
| `0x181` | 1 | `u8` | Image-stretch flag |
| `0x182` | 32 | ASCII | First sound path |
| `0x1a3` | 4 | `u32` | First sound volume |
| `0x1a7` | 32 | ASCII | Second sound path |
| `0x1c8` | 4 | `u32` | Second sound volume |
| `0x2f5` | 4 | `u32` | Alternate fill color, `AARRGGBB` |
| `0x2f9` | 4 | `u32` | Fill color, `AARRGGBB` |
| `0x305` | 17 | ASCII | Primary font name |
| `0x316` | 17 | ASCII | Secondary font name |
| `0x327` | 4 | `u32` | Caption resource identifier |
| `0x32b` | 4 | `u32` | Tooltip resource identifier |
| `0x32f` | 33 | union | Resource path or table columns |
| `0x3b0` | 33 | ASCII | Widget-art base path |
| `0x431` | 4 | `u32` | Alignment |
| `0x435` | 4 | `u32` | Minimum parameter |
| `0x439` | 4 | `u32` | Maximum parameter |
| `0x43d` | 4 | `u32` | Default parameter |
| `0x441` | 4 | `u32` | Axis or mode parameter |
| `0x445` | 4 | `u32` | Auxiliary parameter |
| `0x459` | 4 | `u32` | Item value |
| `0x45d` | 2 | `i16` | Parent group identifier |
| `0x45f` | 2 | `i16` | Own group identifier |
| `0x461` | 2 | `i16` | Parent record index |
| `0x463` | 2 | `i16` | Anchor code |
| `0x46d` | 4 | `u32` | Runtime slot, zero on disk |

Nine 33-byte style slots start at `0x1cc`.
Each slot contains a color string, an asset path, or zeros.

Color strings use the form `0xRRGGBBAA`.
The slot type depends on the widget type.

## Table columns

Type 19 uses packed five-byte column records at `0x32f`.
Other widget types use this location as an ASCII resource path.

| Relative offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `i16` | Column width, where `-1` ends the array |
| `0x02` | `i8` | Alignment, `0`, `1`, or `2` |
| `0x03` | `i16` | Horizontal anchor |

## Anchors

Anchor code `100` selects top-left.
Codes `101`, `102`, and `103` select top-right, bottom-left, and bottom-right.

An anchor code of `-1` means no anchor.
A parent index of `-1` selects the outer viewport.

## Common widget types

| Code | Type |
| ---: | --- |
| 0 | Button |
| 1 | Navigation button |
| 2 | Radio, checkbox, or tab item |
| 4 | Single-line text input |
| 5 | Text label |
| 6 | Filled rectangle |
| 8 | Wrapped text |
| 9 | Image |
| 12 | Slider track |
| 13 | Spinner button |
| 14 | Slider |
| 15 | Scrollbar |
| 16 | List box |
| 17 | Drop-down list |
| 18 | Item group |
| 19 | Table |
| 22 | Layout group |
| 26 | Multiline text input |

## Inspect a layout

Run the dump tool:

```text
opennr_lyt_dump file.lyt
```
