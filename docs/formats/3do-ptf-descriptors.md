# 3DO and PTF descriptor bodies

This reference describes every supported descriptor body.
Fields appear in wire order after the common descriptor header.

Read [3DO and PTF typed object stream](3do-ptf.md) first.
It defines object references, class references, strings, and the common header.

## Notation

| Notation | Meaning |
| --- | --- |
| `object` | One typed object reference |
| `object[count]` | A sequence of typed object references |
| `lp_string` | `u32` byte count, text bytes, and a zero terminator |
| `sized T[count]` | `u32` allocation size followed by the items |
| `bbox` | Six `f64` values that form a 48-byte bounding box |
| `raw[N]` | An uninterpreted byte sequence |

The allocation size is zero when a sized array is absent.
Unless stated otherwise, its value must equal `count * sizeof(T)`.

## Basic descriptors

### EmptyDescriptor

This descriptor has no class body.
The common descriptor header is the complete object.

### NodeDescriptor

| Type | Field | Constraint |
| --- | --- | --- |
| `u32` | `magic` | Expected value is `1` |

### ChildNodeDescriptor

This descriptor has the same body as `NodeDescriptor`.

### VertexListDescriptor

This descriptor has the same body as `NodeDescriptor`.

## Scene graph descriptors

### GroupDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `flag_a` | Parent flag |
| `u32` | `flag_b` | Parent flag |
| `bbox` | `bounds` | Object bounds |
| `u32` | `magic` | Class version |
| `u32` | `child_count` | Number of children |
| `u32` | `child_allocation_size` | Must equal `child_count * 4` |
| `object[child_count]` | `children` | Child objects |

### LodSwitchDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `flag_a` | Parent flag |
| `u32` | `flag_b` | Parent flag |
| `bbox` | `bounds` | Object bounds |
| `u32` | `magic` | Class version |
| `u32` | `level_count` | Number of distance levels |
| `f64[3]` | `center` | Switch center |
| `u8` | `enabled` | Present when `magic > 1` |
| `u32` | `distance_allocation_size` | Must equal `level_count * 8` |
| `f64[level_count]` | `distances` | Level thresholds |
| `u32` | `child_allocation_size` | Must equal `level_count * 4` |
| `object[level_count]` | `children` | Level objects |

### StateSwitchDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `flag_a` | Parent flag |
| `u32` | `flag_b` | Parent flag |
| `bbox` | `bounds` | Object bounds |
| `u32` | `magic` | Class version |
| `lp_string` | `state_name` | State channel name |
| `u8` | `enabled` | Present when `magic >= 2` |
| `u32` | `state_count` | Number of state objects |
| `f64` | `default_state` | Default state value |
| `u32` | `value_allocation_size` | Must equal `(state_count + 1) * 8` |
| `f64[state_count + 1]` | `state_values` | State boundaries or values |
| `u32` | `child_allocation_size` | Must equal `state_count * 4` |
| `object[state_count]` | `children` | State objects |

### GroupingNodeDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `flag_a` | Parent flag |
| `u32` | `flag_b` | Parent flag |
| `bbox` | `bounds` | Object bounds |
| `u32` | `magic` | Class version |
| `u32` | `child_count` | Number of children |
| `f64[3]` | `center` | Group center |
| `u8` | `enabled` | Present when `magic > 1` |
| `u32` | `distance_allocation_size` | Must equal `child_count * 8` |
| `f64[child_count]` | `distances` | Child distance values |
| `u32` | `child_allocation_size` | Must equal `child_count * 4` |
| `object[child_count]` | `children` | Child objects |

### TransformDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `flag_a` | Parent flag |
| `u32` | `flag_b` | Parent flag |
| `bbox` | `bounds` | Object bounds |
| `u32` | `magic` | Class version |
| `f64[3]` | `translation` | X, Y, and Z translation |
| `f64[3]` | `rotation` | Yaw, pitch, and roll |
| `object` | `child` | Transformed object |

### AnimatedTransformDescriptor

The first fields match `TransformDescriptor`.
The second `f64[3]` group stores a rotation axis for this class.

| Type | Field | Meaning |
| --- | --- | --- |
| Transform body | `transform` | Parent fields and child object |
| `u32` | `magic` | Animation version |
| `lp_string` | `channel_name` | Animation channel |
| `u32` | `key_count` | Number of keys |
| `u32` | `key_allocation_size` | Must equal `key_count * 32` |
| key records | `keys` | Consecutive 32-byte keys |

Each key has this structure:

| Offset | Type | Field |
| ---: | --- | --- |
| `0x00` | `u32` | Time value |
| `0x04` | `f32[4]` | Quaternion |
| `0x14` | `f32[3]` | Translation |

### BillboardDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `flag_a` | Parent flag |
| `u32` | `flag_b` | Parent flag |
| `bbox` | `bounds` | Object bounds |
| `u32` | `magic` | Class version |
| `f64[3]` | `pivot` | Pivot position |
| `f64[3]` | `axis` | Facing axis |
| `object` | `child` | Billboard object |

### SelfLightingDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `flag_a` | Parent flag |
| `u32` | `flag_b` | Parent flag |
| `bbox` | `bounds` | Object bounds |
| `u32` | `magic` | Class version |
| `u8` | `diffuse_set` | Diffuse override is present |
| `u8` | `specular_set` | Specular override is present |
| `u8` | `ambient_set` | Ambient override is present |
| `f64[3]` | `diffuse` | Diffuse RGB values |
| `f64[3]` | `specular` | Specular RGB values |
| `f64[3]` | `ambient` | Ambient RGB values |
| `object` | `child` | Affected object |

### AppNodeDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `flag_a` | Parent flag |
| `u32` | `flag_b` | Parent flag |
| `bbox` | `bounds` | Object bounds |
| `u32` | `magic` | Class version |
| `u32` | `application_id` | Application-defined identifier |
| `u32` | `data_size` | Payload size |
| `raw[data_size]` | `data` | Application-defined payload |
| `object` | `child` | Child object |

### PortalDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `node_magic` | Node version |
| `u32` | `magic` | Portal version |
| `object` | `target` | Portal target |
| `u32` | `index_count` | Number of indices |
| `u32` | `index_allocation_size` | Must equal `index_count * 4` |
| `u32[index_count]` | `indices` | Portal polygon indices |

## Geometry descriptors

### GeometryDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `magic` | Class version |
| `object` | `vertex_list` | Vertex data object |
| `object` | `primitive` | First primitive object |

The primitive object can link to another primitive.

### ShapeDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `node_magic` | Node version |
| `u32` | `magic` | Shape version |
| `object` | `appearance` | Material object |
| `object` | `geometry` | Geometry object |

### TriListDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `flag_a` | Primitive flag |
| `object` | `next_primitive` | Next primitive or null |
| `u32` | `magic` | Primitive version |
| `u32` | `index_count` | Number of vertex indices |
| `u32` | `index_allocation_size` | Must equal `index_count * 4` |
| `u32[index_count]` | `indices` | Triangle-list indices |

### TriStripDescriptor

This descriptor has the same body as `TriListDescriptor`.
The index sequence defines a triangle strip.

### TriFanDescriptor

This descriptor has the same body as `TriListDescriptor`.
The index sequence defines a triangle fan.

### PlainVertexListDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `type_code` | Vertex-list type |
| `u32` | `flag` | Vertex-list flag |
| `i32` | `vertex_count` | Number of vertices |
| seven sized arrays | `base_channels` | Seven `f64[vertex_count]` channels |
| `u32` | `attribute_magic` | Attribute version, from 0 through 3 |
| 24 sized arrays | `attributes` | Twenty-four `f64[vertex_count]` channels |

The seven base channels are position X, Y, Z, reserved, and normal X, Y, Z.
Attribute channels zero and one normally contain texture U and V values.

Every channel starts with its own allocation size.
An absent channel has an allocation size of zero.

### TextureCoordsDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `magic` | Expected value is `1` |
| `u32` | `vertex_count` | Number of vertices |
| `u32` | `attribute_magic` | Attribute version, from 0 through 3 |
| four sized arrays | `channels_0_3` | Always present in the layout |
| four sized arrays | `channels_4_7` | Present when `attribute_magic > 2` |
| 16 sized arrays | `channels_8_23` | Always present in the layout |

Each channel contains `f64[vertex_count]` values or has a zero allocation size.

### MorphVertexListDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `magic` | Expected value is `1` |
| `u32` | `morph_magic` | Expected value is `2` |
| `u32` | `max_vertex_count` | Maximum vertices in one frame |
| `u32` | `frame_count` | Number of morph frames |
| grouped arrays | `channels` | Thirty-one per-frame `f64` channels |

The channels form eight groups with counts `4, 3, 4, 4, 4, 4, 4, 4`.
Each channel starts with an allocation size for its frame-pointer table.

Each frame then stores one sized `f64[max_vertex_count]` array per channel.
Zero allocation sizes identify absent frame channels.

### LodMorphVertexListDescriptor

This descriptor starts with the complete `MorphVertexListDescriptor` body.
It then stores these fields:

| Type | Field | Constraint |
| --- | --- | --- |
| `u32` | `lod_magic` | Expected value is `1` |
| sized `f64[frame_count]` | `lod_values` | One value for each frame |

### StateMorphVertexListDescriptor

This descriptor has the same wire layout as `LodMorphVertexListDescriptor`.
The final values select states instead of distance levels.

### RegionMorphVertexListDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `magic` | Expected value is `1` |
| `u32` | `region_magic` | Expected value is `1` |
| `u32` | `vertex_count` | Base vertex count |
| eight sized arrays | `base_channels` | Eight `f64[vertex_count]` channels |
| attribute block | `attributes` | Plain vertex-list attribute trailer |
| `u32` | `region_count` | Number of named regions |
| region records | `regions` | Consecutive region records |

The attribute trailer starts with `attribute_magic`.
It has 20 channels for old versions and 24 channels for version 3.

Each region record has this structure:

| Type | Field |
| --- | --- |
| `lp_string` | Region name |
| `u32` | First region value |
| `u32` | Second region value |
| `u32` | Region vertex count |
| eight sized `f64` arrays | Region channels |
| sized `u32` array | Base vertex indices |

### SpanningVertexListDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `magic` | Expected value is `1` |
| `u32` | `spanning_magic` | Expected value is `1` |
| `u32` | `max_vertex_count` | Maximum vertex count |
| `u32` | `object_count` | Number of source objects |
| `u32` | `object_allocation_size` | Must equal `object_count * 4` |
| `object[object_count]` | `objects` | Source objects |
| per-object arrays | `channels` | Eight channel families |
| attribute block | `attributes` | Plain vertex-list attribute trailer |

The channel families contain one group, four related groups, and three final groups.
Each object has one sized `f64[max_vertex_count]` array in each family.

### BiCubicPatchDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `primitive_magic` | Primitive parent version |
| `object` | `next_primitive` | Next primitive or null |
| `u32` | `magic` | Patch version |
| `f32[16]` | `scalar_grid` | Four-by-four scalar grid |
| `f64[2]` | `parameters` | Two patch parameters |
| `f32[4]` | `vector_a` | First vector |
| `f32[4]` | `vector_b` | Second vector |

### ProgressiveModificationDescriptor

| Type | Field |
| --- | --- |
| `i32` | Vertex-count change |
| `i32` | Modified vertex count |
| `i32` | Triangle-count change |
| `i32` | Modified triangle count |

### ProgressiveMeshDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `node_magic` | Node version |
| `u32` | `magic` | Mesh version |
| `object[3]` | `sources` | Three source objects |
| `u32` | `base_vertex_count` | Base vertex count |
| `u32` | `base_triangle_count` | Base triangle count |
| `u32` | `modification_count` | Number of modifications |
| `u32` | `modification_allocation_size` | Must equal `modification_count * 4` |
| `object[modification_count]` | `modifications` | Modification objects |

## Material and light descriptors

### TextureDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `magic` | Texture version |
| `lp_string` | `texture_name` | Texture resource name |
| `u8` | `flags` | Texture control byte |

### AppearanceDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `magic` | Appearance version |
| `object[6]` | `texture_slots` | Six texture objects |
| `object` | `texture_slot_7` | Present when `magic >= 2` |
| `f64[3]` | `ambient` | Ambient RGB values |
| `f64[3]` | `diffuse` | Diffuse RGB values |
| `f64[3]` | `specular` | Specular RGB values |
| `f64` | `shininess` | Shininess value |
| `f64` | `reflectivity` | Reflectivity value |
| `f64` | `opacity` | Opacity value |
| `f32` | `environment_index` | Environment-map index |

### PointLightDescriptor

| Type | Field | Version rule |
| --- | --- | --- |
| `u32` | `node_magic` | All versions |
| `u32` | `magic` | Accepted range is 0 through 3 |
| `f64` | `radius_squared` | Present when `magic >= 2` |
| `f64[4]` | `legacy_color` | Present when `magic < 3` |
| `f64` | `diffuse_scale` | Present when `magic < 3` |
| `f64` | `ambient_scale` | Present when `magic < 3` |
| `f32[3]` | `diffuse` | Present when `magic >= 3` |
| `f32[3]` | `ambient` | Present when `magic >= 3` |
| `f64[3]` | `position` | All versions |

### InfiniteLightDescriptor

| Type | Field | Version rule |
| --- | --- | --- |
| `u32` | `node_magic` | All versions |
| `u32` | `magic` | Accepted range is 0 through 2 |
| `f64[4]` | `legacy_color` | Present when `magic < 2` |
| `f64` | `diffuse_scale` | Present when `magic < 2` |
| `f64` | `ambient_scale` | Present when `magic < 2` |
| `f32[3]` | `diffuse` | Present when `magic >= 2` |
| `f32[3]` | `ambient` | Present when `magic >= 2` |
| `f64[3]` | `axis` | All versions |

## Track and PTF descriptors

### TrackDescriptor

| Type | Field | Version rule |
| --- | --- | --- |
| `u32` | `magic` | Accepted range is 0 through 8 |
| `i32` | `segment_count` | All versions |
| `u8` | `flag` | Present when `magic >= 3` |
| `f64[3]` | `scalars_a_c` | Present in version 8 |
| `u32` | `scalar_d` | Present in version 8 |
| legacy `f64` values | `legacy_values` | Present in versions 3 through 7 |
| raw records | `records_e` | Present when `magic >= 7` |
| object groups | `references` | Version-dependent reference groups |
| `object[segment_count]` | `segments` | Segment objects |

The legacy value count is five in versions 3 and 4.
Version 5 adds eight values, and version 6 adds another eight values.

The version 7 and 8 raw-record group starts with a count and allocation size.
Each raw record has 48 bytes.

The reference groups use a count, an allocation size, and object references.
The final segment group uses `segment_count * 4` as its allocation size.

Versions 2, 4, 5, 6, 7, and 8 store `single_child` first.
They then store `children_c` as a counted object group.

All versions store `children_b` as a counted object group.
The segment allocation size and segment objects follow that group.

### SegmentDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `magic` | Accepted values are 1 and 2 |
| three object groups | `x_sections`, `f_sections`, `w_sections` | Three counted object-reference arrays |
| `i32` | `segment_kind` | Segment type, or `-1` for no remaining body |
| `f64[6]` | `parameters` | Position and angle values |
| `u8[2]` | `flags` | Segment flags |
| sized 12-byte records | `records_12` | First raw record array |
| sized 16-byte records | `records_16` | Second raw record array |

Each object group stores a count.
A nonempty group then stores `count * 4` and its object references.

### TrackDetailDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `magic` | Detail version |
| `object` | `child` | Detail object |
| `f64[5]` | `parameters` | Five detail values |
| `u32` | `block_a_size` | Expected value is `32` |
| `raw[32]` | `block_a` | First fixed block |
| `u32` | `block_b_size` | Expected value is `32` |
| `raw[32]` | `block_b` | Second fixed block |

### TrackGrooveDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `magic` | Groove version |
| `u32` | `sample_count` | Number of samples |
| `object` | `child` | Related object |
| `f64` | `scalar` | Groove parameter |
| five sized arrays | `channels` | Five `f64[sample_count]` channels |

### TrackRaceLineDescriptor

| Type | Field | Meaning |
| --- | --- | --- |
| `u32` | `magic` | Race-line version |
| `u32` | `sample_count` | Number of samples |
| `object` | `child` | Related object |
| `f64[3]` | `scalars` | Three race-line parameters |
| two sized arrays | `channels` | Two `f64[sample_count]` channels |

### TSODescriptor

| Type | Field | Version rule |
| --- | --- | --- |
| `u32` | `magic` | Accepted range is 0 through 3 |
| `lp_string` | `name_a` | All versions |
| `lp_string` | `name_b` | Present when `magic >= 2` |
| `u8` | `flag` | Present when `magic >= 3` |

### TSOReferenceDescriptor

| Type | Field | Version rule |
| --- | --- | --- |
| `u32` | `magic` | Accepted range is 0 through 6 |
| `f64[6]` | `transform` | All versions |
| `object` | `child` | All versions |
| `u32` | `conditional_flag` | Present when `magic >= 4` |
| `u8` | `has_extra` | Present after the version gate |
| `raw[44]` | `inline_payload` | Present under the inline-payload condition |
| `u32` | `extra_count` | Present when `magic != 2` |
| `raw[extra_count * 40]` | `extra_records` | Forty-byte records |
| `lp_string` | `name` | Present when `magic >= 6` |

The inline payload is present before version 5.
For version 5 or later, the one-byte payload gate controls its presence.

Version 1 ends after the child object.
It does not store the control byte, inline payload, extra records, or name.

### X_SectionDescriptor

| Type | Field | Version rule |
| --- | --- | --- |
| `u32` | `magic` | Accepted range is 1 through 3 |
| scalar | `lateral_start` | `f64` before version 3, otherwise `f32` |
| `f64` | `height_start` | All versions |
| scalar | `slope_start` | `f64` before version 3, otherwise `f32` |
| scalar | `lateral_end` | `f64` before version 3, otherwise `f32` |
| `f64` | `height_end` | All versions |
| scalar | `slope_end` | `f64` before version 3, otherwise `f32` |
| `u8` | `visual_curve_mode` | Present when `magic >= 2` |
| seam blocks | `start_seam`, `end_seam` | Present in version 3 |

Each seam block starts with a `u8` kind.
A nonzero kind adds two `f32` parameters.
Kind 2 also adds two `u8` boundary indices.

### F_SectionDescriptor

| Type | Field | Version rule |
| --- | --- | --- |
| `u32` | `magic` | Accepted range is 1 through 5 |
| scalar | `lateral_start` | `f64` before version 5, otherwise `f32` |
| scalar | `lateral_end` | `f64` before version 5, otherwise `f32` |
| `u8` | `boundary_mode` | Present when `magic >= 3` |
| `u32` | `query_tag` | Present when `magic >= 4` |
| `u32` | `surface_code` | Surface identifier |
| `object` | `x_section` | Cross-section object |
| `object` | `w_section` | Present when the source gate is nonzero |
| six optional blocks | `trailers` | Present after the conditional `w_section` path |

The source object determines which trailer blocks exist.
Each existing trailer starts with a one-byte gate.
A zero gate adds two flagged `f64` values and three more `f64` values.

### W_SectionDescriptor

| Type | Field | Version rule |
| --- | --- | --- |
| `u32` | `magic` | Accepted range is 1 through 7 |
| scalar | `lateral_start` | `f64` before version 6, otherwise `f32` |
| scalar | `lateral_end` | `f64` before version 6, otherwise `f32` |
| `u8` | `boundary_mode` | Present when `magic >= 3` |
| `u32` | `query_tag` | Present when `magic >= 4` |
| `f64[6]` | `profile_values` | Heights, visual offsets, and collision thicknesses |
| `i32` | `height_offset_mode` | Height mode |
| `u32` | `wall_profile_kind` | Present when `magic >= 7` |
| `raw[40]` | `wall_profile` | Present when `wall_profile_kind == 1` |
| `u32` | `surface_code` | Surface identifier |
| `u32` | `record_count` | Present when `magic >= 5`, otherwise one record |
| records | `records` | Consecutive section records |

Each record starts with an `f64` longitudinal position in version 5 or later.
Five inner pairs follow.

Each pair starts with one child object.
A nonzero source gate adds a second child and up to six interpolation blocks.

The source object determines which interpolation blocks exist.
Each existing block starts with a `u8` gate.
A zero gate adds two entries with a flag, rate, and offset.
The rate and offset fields use `f64` values.
