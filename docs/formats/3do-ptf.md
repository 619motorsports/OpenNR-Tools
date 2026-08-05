# 3DO and PTF typed object stream

`.3do` and `.ptf` files contain a typed graph of persistent objects.
The stream has no separate file header.

## Object framing

Every object reference starts with an object handle:

| Field | Type | Meaning |
| --- | --- | --- |
| `object_handle` | `u32` | Zero, a back-reference, or a new object |
| `class_handle` | `u32` | A known class or a new class |
| `class_name_length` | `u32` | New class-name size with its zero terminator |
| `class_name` | bytes | New class name |
| body | bytes | Data for the selected class |

An object handle of zero is a null reference.
A known object handle refers to an earlier object.

A new object handle is followed by a class handle.
A known class handle selects an earlier class name.
A new class handle is followed by its class name.

Object handles and class handles use separate tables.
The reader must register a new object before it reads that object body.
This order permits cycles and shared references.

Fields have no external alignment.
Read all integer and floating-point fields as little-endian values.

## Descriptor header

Most descriptor bodies start with this common header:

| Relative offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u32` | Descriptor version |
| `0x04` | `u32` | Name size with its zero terminator |
| `0x08` | bytes | Optional name |

Class-specific fields start after the optional name.

## Descriptor bodies

The stream supports 43 descriptor classes.
Each class has a defined body after the common descriptor header.

Read the [descriptor body reference](3do-ptf-descriptors.md) for every class layout.
That reference lists fields in wire order and includes all version gates.

## Sized arrays

Many descriptor arrays start with a `u32` allocation size.
This value is the array size in bytes.

A zero allocation size means that the array is absent.
Object arrays store four bytes per object handle.
`f64` arrays store eight bytes per item.

Readers must compare each allocation size with its count and element size.
Some old versions store allocation data that has no known field meaning.

## Inspect a stream

Inspect an extracted file:

```text
opennr_object_tool model.3do --tree
```

Inspect an entry without extracting its DAT archive:

```text
opennr_object_tool archive.dat entry-name --tree
```
