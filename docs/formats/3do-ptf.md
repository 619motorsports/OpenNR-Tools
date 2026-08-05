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

## Common class groups

Scene classes include `GroupDescriptor`, `LodSwitchDescriptor`, and `TransformDescriptor`.
Geometry classes include `ShapeDescriptor`, `GeometryDescriptor`, and vertex-list descriptors.

Primitive classes include `TriListDescriptor`, `TriStripDescriptor`, and `TriFanDescriptor`.
Material classes include `AppearanceDescriptor` and `TextureDescriptor`.

PTF streams add track, segment, section, detail, and track-side-object descriptors.

## Selected class layouts

### TextureDescriptor

After the common header, this class stores a version and a sized texture name.
Version 2 also stores one load-flags byte.

### GeometryDescriptor

After the common header, this class stores its version and two object references.
The references identify a vertex list and the first primitive.

### PlainVertexListDescriptor

The body contains the vertex count and separate arrays for each coordinate.
Each `f64` array starts with its byte size.

Position arrays are followed by a reserved array and three normal arrays.
An attribute version and 24 sized attribute arrays follow the normals.

Attribute arrays zero and one commonly contain texture coordinates.
An absent attribute has a zero byte size.

### Primitive descriptors

All three primitive classes use the same serialized layout.
The body stores a next-primitive object, a version, and a sized `u32` index array.

### AppearanceDescriptor

The body stores six texture objects.
Version 2 adds a seventh texture object.

Three `f64[3]` color groups follow the texture objects.
Shininess, reflectivity, opacity, and one `f32` environment index follow the colors.

### TrackDescriptor

This class is the root of a PTF stream.
Its body stores a version, a segment count, and segment objects.

## Inspect a stream

Inspect an extracted file:

```text
opennr_object_tool model.3do --tree
```

Inspect an entry without extracting its DAT archive:

```text
opennr_object_tool archive.dat entry-name --tree
```
