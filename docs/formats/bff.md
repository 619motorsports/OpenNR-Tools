# BFF bitmap-font format

A `.bff` file contains glyph metrics, an atlas table, and a compressed bitmap.
The file uses nested chunks.

## Chunk header

Tags use reversed byte order on disk.
Logical tag `PFNT` appears as `T N F P`.

Every chunk uses this 12-byte header:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 4 | FourCC | Reversed tag bytes |
| `0x04` | 4 | `u32` | Reserved and normally zero |
| `0x08` | 4 | `u32` | Body size |

Chunk bodies use `0x20` padding to reach a four-byte boundary.

## Chunk tree

```text
PFNT
  PFHD
  PFGD
  STMP
    STHD
    DATA
    BMAP
      BMHD
      DATA
```

## PFHD body

`PFHD` has a six-byte body:

| Offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u16` | Line height in pixels |
| `0x02` | `u16` | Baseline-related metric |
| `0x04` | `u16` | Flags |

## PFGD body

The body starts with a `u16` glyph count.
A variable-size descriptor follows for each glyph.

| Offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u16` | Unicode code point |
| `0x02` | `f32` | Left bearing in pixels |
| `0x06` | `f32` | Pen advance in pixels |
| `0x0a` | `u16` | Reserved |
| `0x0c` | `u16` | Extent metric |
| `0x0e` | `i16` | Descent in pixels |
| `0x10` | `u16` | Kerning-pair count |

Each kerning pair contains a `u16` next code point and an unaligned `f32` adjustment.
The next glyph descriptor follows the final kerning pair.

## STMP body

`STHD` contains a `u32` glyph count.
The adjacent `DATA` body contains one 12-byte atlas record per glyph.

| Offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u16` | Reserved |
| `0x02` | `u16` | Atlas Y position |
| `0x04` | `u16` | Glyph width |
| `0x06` | `u16` | Glyph height |
| `0x08` | `u32` | Packed advance and reserved value |

The atlas is a vertical strip.
Each glyph starts at X position zero and its recorded Y position.

The high 16 bits of the packed value contain an integer pen advance.
The low 16 bits are reserved.

The `f32` advance in `PFGD` is the more precise value.

## BMAP body

`BMAP` contains a 14-byte `BMHD` body and one compressed `DATA` body.

| Offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u8` | Format tag |
| `0x01` | `u8` | Atlas width |
| `0x02` | bytes | Reserved fields and encoded height |
| `0x05` | `u24` | Atlas height |
| `0x0c` | `u16` | Pixel-format value |

The `DATA` body uses PKWARE DCL compression.
Decoded pixels are row-major, eight-bit grayscale alpha values.

The decoded size is `atlas_width * atlas_height` bytes.
Known files use the high nibble for 16 alpha levels.
The low nibble is zero.

## Inspect and render

Display metrics and optionally write the raw atlas:

```text
opennr_bff_inspect font.bff atlas.gray
```

Render text to a BMP file:

```text
opennr_bff_render_text font.bff "Sample" sample.bmp
```
