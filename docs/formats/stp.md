# STP stamp-image format

An `.stp` file contains one image or a horizontal strip of sub-images.
The pixel body can be raw or PKWARE DCL-compressed.

## Chunk header

Tags use reversed byte order on disk.
Logical tag `STMP` appears as `P M T S`.

Every chunk uses this 12-byte header:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 4 | FourCC | Reversed tag bytes |
| `0x04` | 4 | `u32` | Reserved and normally zero |
| `0x08` | 4 | `u32` | Body size |

## Chunk tree

```text
STMP
  STHD
  DATA
  BMAP
    BMHD
    DATA
```

## STHD and outer DATA

`STHD` contains one `u32` sub-image count.
The outer `DATA` body contains one 12-byte record per sub-image.

| Offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u32` | Reserved |
| `0x04` | `u16` | Sub-image width |
| `0x06` | `u16` | Sub-image height |
| `0x08` | `u32` | Reserved |

For multiple images, use these dimensions to split the BMAP horizontally.

## BMHD body

`BMHD` has a 14-byte body:

| Offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u8` | Pixel-format code |
| `0x01` | `u32` | Total width in pixels |
| `0x05` | `u32` | Total height in pixels |
| `0x09` | `u32` | Row pitch in bytes |
| `0x0d` | `u8` | Compression flag |

Two `0x20` bytes can separate `BMHD` from the inner `DATA` chunk.

## Pixel data

A compression flag of zero means that the payload is raw.
A compression flag of one selects PKWARE DCL compression.

The decoded payload size is `pitch * height` bytes.
The row pitch uses four-byte alignment.

Known pixel-format codes are listed below:

| Code | Bytes per pixel | Layout |
| ---: | ---: | --- |
| `0x03` | 2 | RGB565 |
| `0x05` | 2 | ARGB4444 |
| `0x06` | 3 | RGB888 byte order |
| `0x07` | 4 | BGRA8888 byte order |

Rows can contain padding after the final pixel.
Use `pitch`, not `width * bytes_per_pixel`, to locate the next row.

## Decode an image

Write the decoded image to a BMP file:

```text
opennr_stp_dump input.stp output.bmp
```
