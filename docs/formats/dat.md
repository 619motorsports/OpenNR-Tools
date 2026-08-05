# DAT archive format

A `.dat` file contains named entries and their byte payloads.
An entry can contain stored bytes or a PKWARE DCL-compressed stream.

## File header

The file starts with this 10-byte header:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 4 | `u32` | Archive hash |
| `0x04` | 6 | bytes | Reserved and normally zero |

The table of contents starts at offset `0x0a`.
It ends at the smallest data offset in the table.

## Table entry

Each table entry has a variable size:

| Relative offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| `0x00` | 2 | `u16` | Flags |
| `0x02` | 4 | `u32` | Uncompressed size |
| `0x06` | 4 | `u32` | Stored size |
| `0x0a` | 4 | `u32` | Absolute data offset |
| `0x0e` | 1 | `u8` | Name length without the terminator |
| `0x0f` | N | bytes | Entry name |
| `0x0f + N` | 1 | `u8` | Zero terminator |

The total entry size is `16 + name_length` bytes.
Bit `0x0200` marks a compressed entry.
Known valid entries also contain the low flag mask `0x0005`.

Readers must not treat other flag bits as compression indicators.
Writers must retain unknown bits when they update an entry.

Names are relative paths and can contain backslashes.
A safe extractor must reject absolute paths and parent-directory components.

## Entry data

Read `stored_size` bytes from `data_offset`.
For an uncompressed entry, these bytes are the complete output.

For a compressed entry, decode the bytes to `uncompressed_size`.
The compressed data uses the PKWARE Data Compression Library implode format.

## DCL stream

The stream starts with two bytes:

| Offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u8` | Literal mode, `0` or `1` |
| `0x01` | `u8` | Extra distance bits, `4`, `5`, or `6` |

The remaining stream uses least-significant-bit-first codes.
A zero flag introduces a literal byte.
A one flag introduces a length and distance pair.
A decoded length of `519` ends the stream.

The length code uses a 16-symbol Huffman table.
Its symbol selects a base length and a count of extra bits.

The distance code uses a 64-symbol Huffman table.
A length of two uses two low distance bits.
Other lengths use the stream's extra-distance-bit value.

Calculate the distance as `(symbol << low_bit_count) | low_value`, then add one.
Copy the decoded length from that distance behind the output cursor.

The dictionary size is `64 << extra_distance_bits` bytes.
Therefore, values 4, 5, and 6 select 1, 2, and 4 KiB dictionaries.

## Inspect and extract

List an archive:

```text
opennr_dat_tool list archive.dat
```

Extract all entries:

```text
opennr_dat_tool extract archive.dat output-directory
```

Extract one entry:

```text
opennr_dat_tool extract-one archive.dat entry-name output-file
```

On Windows, run `opennr_dat_tool` without arguments to open the archive browser.
