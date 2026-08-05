# CAR file format

A `.car` file is a chunked container.
Its common chunks contain a type word, INI text, and two texture payloads.

## FourCC byte order

Tags use reversed byte order on disk.
For example, logical tag `CARF` appears as the bytes `F R A C`.

## Outer chunk

The file starts with one `CARF` chunk:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 4 | FourCC | `CARF` |
| `0x04` | 4 | `u32` | Reserved and normally zero |
| `0x08` | 4 | `u32` | Body size |
| `0x0c` | N | bytes | Child chunks |

The body size normally equals `file_size - 12`.

## Child chunk header

Each child uses this 12-byte header:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 4 | FourCC | Reversed tag bytes |
| `0x04` | 4 | `u32` | Reserved and normally zero |
| `0x08` | 4 | `u32` | Body size |

Two `0x20` bytes can separate sibling chunks.
The padding is not part of either chunk body.

## Common child chunks

The common order is `CTYP`, `CINI`, `CTEX`, and `CREW`.

### CTYP

`CTYP` contains one little-endian `u32` value.
Bit 31 is a flag.
The low 31 bits contain a class value.

### CINI

`CINI` contains ASCII INI text with CRLF line endings.
The common sections are listed below:

- `[AIParamDeviation]`
- `[AIParamMean]`
- `[CareerStats]`
- `[CurrentYearStats]`
- `[Driver]`

The first two sections use these keys:

```text
aiParamDriverAggression
aiParamDriverConsistency
aiParamDriverFinishing
aiParamDriverQualifying
aiParamDriverRoadCourse
aiParamDriverShortTrack
aiParamDriverSpeedway
aiParamDriverSuperspeedway
aiParamPitcrewConsistency
aiParamPitcrewSpeed
aiParamPitcrewStrategy
aiParamVehicleAero
aiParamVehicleChassis
aiParamVehicleEngine
aiParamVehicleReliability
```

`[CareerStats]` and `[CurrentYearStats]` use the same seven integer keys:

```text
NumChampionships
NumDNF
NumStarts
NumTop10
NumTop5
NumWins
Winnings
```

`[Driver]` uses these text and integer fields:

| Key | Value |
| --- | --- |
| `birth_date` | Text date or an empty value |
| `car_class` | Numeric class identifier |
| `car_make` | Numeric manufacturer identifier |
| `car_number` | Display number text |
| `first_name` | Driver first name |
| `home_town` | Home-town text or an empty value |
| `last_name` | Driver last name |
| `sponsor` | Sponsor text |
| `team_name` | Team text |

### CTEX and CREW

`CTEX` and `CREW` contain MIP-format texture data.
Each payload starts with a 32-byte MIP header.
Nested `BMAP`, `BMHD`, and `DATA` chunks follow that header.

`CTEX` stores the vehicle texture.
`CREW` stores a separate crew texture.

### Embedded MIP header

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 1 | `u8` | Signature byte, expected `0x04` |
| `0x01` | 1 | `u8` | Signature byte, expected `0x00` |
| `0x02` | 1 | `u8` | Base pixel format |
| `0x03` | 1 | `u8` | Address flags |
| `0x04` | 4 | `u32` | Base-2 width exponent |
| `0x08` | 4 | `u32` | Base-2 height exponent |
| `0x0c` | 4 | `u32` | `AARRGGBB` color key |
| `0x10` | 4 | `u32` | Mipmap count |
| `0x14` | 4 | `u32` | Texture option |
| `0x18` | 4 | `f32` | Texture factor |
| `0x1c` | 4 | `u32` | Reserved |

Address flag bit zero clamps the U axis.
Address flag bit one clamps the V axis.
A clear bit selects wrapping for that axis.

The base width is `1 << width_exponent`.
The base height is `1 << height_exponent`.

### Embedded mipmap levels

The `BMAP` levels follow the 32-byte header from smallest to largest.
Each `BMAP` contains a `BMHD` chunk and a `DATA` chunk.

The 14-byte `BMHD` body has this structure:

| Offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u8` | Pixel-format code |
| `0x01` | `u32` | Level width |
| `0x05` | `u32` | Level height |
| `0x09` | `u32` | Decoded row pitch |
| `0x0d` | `u8` | Compression flag |

A compression flag of zero selects raw bytes.
A value of one selects PKWARE DCL compression.

Two `0x20` bytes can separate `BMHD` and `DATA`.
The level's pixel format controls the decoded data size.

## Inspect a file

Run the inspection tool:

```text
opennr_car_tool file.car
```
