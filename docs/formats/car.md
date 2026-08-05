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

### CTEX and CREW

`CTEX` and `CREW` contain MIP-format texture data.
Each payload starts with a 32-byte MIP header.
Nested `BMAP`, `BMHD`, and `DATA` chunks follow that header.

## Inspect a file

Run the inspection tool:

```text
opennr_car_tool file.car
```
