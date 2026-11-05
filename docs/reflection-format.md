# Reflection format

`enshrouded.exe` embeds a static table describing every one of its data types. This is its layout.
[`tools/reflect_dump.py`](../tools/reflect_dump.py) dumps the table to JSON and reproduces every count below.

- **Build checked:** PE timestamp `1792148057`, log build `ef984944dc330a30f175adaa897d05c7c7ff9ea0`, SHA-256
  `82e5020a8736348cd0374f004b2f5e5ef456b5bd27aa5420df34c7dd241cfeeb`. The layout is unchanged on `1792507660` and
  `1793791052` (12,658 entries there).
- **Addresses** are VAs at the preferred image base `0x140000000` and are build-specific.

## Locating the table

1. Find the NUL-terminated strings `"BlobString"` and `"uint32"` in `.rdata`.
2. Find the 8-aligned qword holding each string's VA: that is offset 0 (the name pointer) of the type's descriptor.
3. Find where those two descriptor VAs sit next to each other: the table starts there, BlobString and uint32 first.
4. Find the qword pointing at the table start. It begins a `{TypeMetadata** types, u64 count}` header.

On `1792148057` the header is at `0x141718a90` and points to the table at `0x1416fd950`: **12,646** entries. Both are
in `.rdata`, so the table is compile-time data.

## TypeMetadata (0x90 bytes)

| Off | Type | Field | Notes |
|---|---|---|---|
| 0x00 | `char*` + `u64` | name | e.g. `Health` |
| 0x10 | `char*` + `u64` | impact_name | The name the game's impact scripting uses |
| 0x20 | `char*` + `u64` | qualified_name | e.g. `keen::ecs::Health` |
| 0x30 | `Namespace*` | namespace | A list going up to the parent namespace |
| 0x38 | type ref | inner_type | Containers: element type. **Structs: base class** |
| 0x40 | `u32` | size | |
| 0x44 | `u16` | alignment | |
| 0x46 | `u16` | element_alignment | |
| 0x48 | `u32` | field_count | For whichever field array is present |
| 0x4C | `u8` | primitive_type | See below |
| 0x4D | `u8` | flags | See below |
| 0x50 | `u32` | qualified_hash | FNV-1a 32 of `qualified_name` |
| 0x54 | `u32` | internal_hash | Meaning unknown |
| 0x58 | `StructFieldMetadata*` | struct_fields | `field_count` entries |
| 0x60 | `EnumFieldMetadata*` | enum_fields | `field_count` entries |
| 0x68 | `TypeMetadata**` | variant_type | Non-null on 25 types |
| 0x70 | `u8*` + `u64` | default_value | Raw default bytes, `size` long |
| 0x80 | `Attribute*` + `u64` | attributes | |

**inner_type on a struct is its base class:** `keen::ecs::Health` and `keen::ecs::Stamina` derive from
`keen::ecs::SInt32Attribute`, and 686 component types chain up to `keen::ecs::Component`.

## Default values

- 1,444 of the 12,646 types have a default value, all structs, always `size` bytes long.
- No `keen::ds::*` type has one, but most have a twin outside `keen::ds` with the same name and fields that does. The
  twin's layout can differ (`keen::ds::g38_options::pc::user::Settings` is 0x2f8 bytes, its twin 0x1e0), because it
  holds blob strings and arrays where the `ds` type holds `Ds` ones. Matched by field name, they give a default for
  every scalar of the game's live options.

## StructFieldMetadata (0x30 bytes)

| Off | Type | Field |
|---|---|---|
| 0x00 | `char*` + `u64` | name |
| 0x10 | type ref (never null) | type |
| 0x18 | `u64` | data_offset inside the owning struct |
| 0x20 | `Attribute*` + `u64` | attributes |

## EnumFieldMetadata (0x28 bytes)

Name (`char*` + `u64`) at 0x00, `u64 value` at 0x10, 16 unused bytes at 0x18.

## Attribute (0x18) and AttributeMetadata (0x20)

- `Attribute`: `AttributeMetadata*` at 0x00, then the value string (`char*` + `u64`) at 0x08.
- `AttributeMetadata`: `Namespace*` at 0x00, name (`char*` + `u64`) at 0x08, a type ref at 0x18.
- Impact node pins are attributes (`impact_node_input`, `impact_node_output`, `impact_config` and others).

## Namespace (0x18 bytes)

Name (`char*` + `u64`) at 0x00, `Namespace* parent` at 0x10, null at the root.

## References, strings, hashes

- A **type ref** is the target descriptor's VA; `0` means none.
- **Strings** are `(ptr, len)` pairs; the length is authoritative.
- **qualified_hash** is FNV-1a 32 (basis `0x811c9dc5`, prime `0x01000193`) of `qualified_name`, for all 12,646 types.
  The game's component and event lookups are keyed by it.

## Primitive type ids

Counts are for build `1792148057`.

| Id | Kind | Count | Id | Kind | Count |
|---|---|---|---|---|---|
| 0x01-0x0A | bool, ints, float32 | 1 each | 0x15 | DsString | 1 |
| 0x0B | float64 | 0 | 0x16 | DsOptional | 8 |
| 0x0C | Enum | 826 | 0x17 | DsVariant | 11 |
| 0x0D-0x10 | Bitmask8/16/32/64 | 86/25/37/6 | 0x18 | BlobArray | 654 |
| 0x11 | Typedef | 1,177 | 0x19 | BlobString | 1 |
| 0x12 | Struct | 8,373 | 0x1A | BlobOptional | 12 |
| 0x13 | StaticArray | 376 | 0x1B | BlobVariant | 14 |
| 0x14 | DsArray | 391 | 0x1C | ObjectReference | 637 |
| | | | 0x1D | Guid | 1 |

## Flags

`HAS_DS` 0x01, `HAS_BLOB_ARRAY` 0x02, `HAS_BLOB_STRING` 0x04, `HAS_BLOB_OPTIONAL` 0x08, `HAS_BLOB_VARIANT` 0x10,
`IS_GPU_UNIFORM` 0x20, `IS_GPU_STORAGE` 0x40, `IS_GPU_CONSTANT` 0x80.

`HAS_DS` is set on 4,801 types, 3,994 of them named `keen::ds::*`; it most likely marks the serialized forms.

## No functions

No qword in any descriptor points into `.text`: the table describes data only, with no methods, constructors or
vtables. Descriptors aren't a fixed stride apart (gaps of 0xB0, 0xC0, 0xE0 are common), so other data sits between
them.

## ECS schema lists

Next to the table header sits a static struct (build `1792148057`):

| VA | Value | Meaning |
|---|---|---|
| `0x141718a90` | `{0x1416fd950, 12646}` | The type table |
| `0x141718aa0` | `{"ecs.component", 13}` | Name |
| `0x141718ab0` | `{0x141716cb0, 687}` | All ECS component types, each a descriptor |
| `0x141718ac0` | `{"ecs.event", 9}` | Name |
| `0x141718ad0` | `{0x141718430, 131}` | All ECS event types, `keen::ecs::Event` included |

The function at `0x1408202e0` (`lea rax, [0x14171a230]; ret`) returns a second struct that points at this one.
