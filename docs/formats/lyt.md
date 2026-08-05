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
| `0x118` | 4 | `u32` | Animation mode |
| `0x11c` | 4 | `u32` | Animation item count |
| `0x120` | 4 | `u32` | Animation flag |
| `0x124` | 4 | `u32` | Animation threshold |
| `0x128` | 4 | `u32` | Animation parameter |
| `0x13c` | 4 | `u32` | Frame count |
| `0x140` | 4 | `f32` | Frame duration |
| `0x150` | 16 | `i32[4]` | First animation position |
| `0x160` | 16 | `i32[4]` | Second animation position |
| `0x170` | 16 | bytes | Preserved animation values |
| `0x180` | 1 | `u8` | Image-animation flag |
| `0x181` | 1 | `u8` | Image-stretch flag |
| `0x182` | 32 | ASCII | First sound path |
| `0x1a3` | 4 | `u32` | First sound volume |
| `0x1a7` | 32 | ASCII | Second sound path |
| `0x1c8` | 4 | `u32` | Second sound volume |
| `0x2f5` | 4 | `u32` | Alternate fill color, `AARRGGBB` |
| `0x2f9` | 4 | `u32` | Fill color, `AARRGGBB` |
| `0x2fd` | 8 | bytes | Reserved |
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
| `0x449` | 8 | ASCII | First extra string |
| `0x451` | 8 | ASCII | Start of second extra string |
| `0x459` | 4 | `u32` | Item value |
| `0x45d` | 2 | `i16` | Parent group identifier |
| `0x45f` | 2 | `i16` | Own group identifier |
| `0x461` | 2 | `i16` | Parent record index |
| `0x463` | 2 | `i16` | Anchor code |
| `0x465` | 8 | bytes | Reserved |
| `0x46d` | 4 | `u32` | Runtime slot, zero on disk |

The second extra string occupies 28 bytes from `0x451`.
Its storage overlaps later typed fields in this tagged union.

Bytes from `0x020` through `0x0ff` are reserved.
Bytes from `0x300` through `0x304` are also reserved.

## Animation block

Static widgets normally set the animation block to zero.
Animated image records use the frame count, duration, and two position records.

Each position record has four signed coordinates.
Interpret them as start X, start Y, end X, and end Y.

The 16 bytes at `0x170` can repeat the frame-duration bit pattern.
Readers must retain these bytes without assigning additional coordinates.

The animation fields at `0x118` through `0x128` are a separate driver block.
Their values control list or credit-sequence animation data.

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

## Widget types and field use

| Code | Type | Important record fields |
| ---: | --- | --- |
| 0 | Button | Style slots, sounds, `param_aux`, linkage |
| 1 | Navigation button | Secondary font, resource identifiers, sounds, linkage |
| 2 | Radio, checkbox, or tab item | Primary font, style slots, art base, `item_value`, linkage |
| 3 | Text radio or tab item | Fonts, styles, resource identifiers, parameter range |
| 4 | Single-line text input | Primary font, alignment, parameter range, linkage |
| 5 | Text label | Primary font, alignment, resource identifier |
| 6 | Filled rectangle | `fill_argb`, geometry, linkage |
| 8 | Wrapped text | Primary font, alignment, `param_default` line spacing |
| 9 | Image | Resource path, image flags, animation block |
| 11 | Graphic helper | Geometry, resource fields, linkage |
| 12 | Slider track | Style slots, range values, `param_axis` |
| 13 | Spinner button | Style slots, step values, `param_axis` |
| 14 | Slider | Range values, axis, style slots, linkage |
| 15 | Scrollbar | Group linkage and type 12 or 13 child records |
| 16 | List box | Primary font, fill colors, `param_axis`, linkage |
| 17 | Drop-down list | Style slots and a type 16 child record |
| 18 | Item group | `own_group_id` and type 0, 2, or 3 children |
| 19 | Table | Column records, font, fill colors, table parameters |
| 21 | Tiled image | Resource path, repeat count, channel-mode parameter |
| 22 | Layout group | `own_group_id` and consecutive children |
| 23 | Text-input variant | Same core fields as type 4 |
| 25 | Image alias | Same fields as type 9 |
| 26 | Multiline text input | Font, capacity, line spacing, alignment |

Codes 7, 10, 20, and 24 are invalid.

### Value parameters

The five `u32` values at `0x431` through `0x445` form a shared parameter block.
Their meaning depends on the widget type.

| Field | Common meanings |
| --- | --- |
| `alignment` | `0` left, `1` right, `2` center |
| `param_min` | Minimum value, text capacity, or image repeat count |
| `param_max` | Maximum value, row cap, or image channel mode |
| `param_default` | Default value, step, or extra line spacing |
| `param_axis` | Axis, direction, or list mode |
| `param_aux` | Button state option or table selection mode |

Type 19 uses `param_aux=1` for row selection.
It uses `param_aux=2` for cell selection.

### Container records

Types 15, 18, and 22 can own consecutive child records.
The container stores a nonzero `own_group_id`.

A child belongs to that container when its `parent_group_id` has the same value.
Children must remain consecutive in file order.

Type 15 uses type 12 children for its track and thumb.
It uses type 13 children for its arrow buttons.

Type 17 normally owns one type 16 list record.

### Text records

Types 4, 5, 8, 16, 19, and 26 use the primary font slot.
Type 1 normally uses the secondary font slot.

Type 26 uses `param_min` as its byte capacity.
A zero capacity selects a default of `0x400` bytes.

Types 8 and 26 use `param_default` as extra line spacing.
The alignment value applies to each displayed line.

### Image records

Types 9 and 25 use the resource path at `0x32f`.
The byte at `0x180` enables authored animation data.

The byte at `0x181` selects stretched output.
When this byte is zero, the image keeps its natural dimensions.

Type 21 repeats its image in both axes.
`param_min` stores the repeat count when a resource path is present.

## Style slots

The nine style slots occupy offsets `0x1cc` through `0x2f4`.
Their starts are `0x1cc + index * 33`.

Each slot stores zero bytes, an asset name, or a `0xRRGGBBAA` color string.
The widget type defines the state represented by each slot.

Binary fill colors use `AARRGGBB` order instead.
Do not swap the byte order between string colors and binary colors.

## Inspect a layout

Run the dump tool:

```text
opennr_lyt_dump file.lyt
```
