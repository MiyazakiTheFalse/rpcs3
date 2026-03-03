# RPCS3 Shared Cache Key Schema (PPU/SPU/RSX)

This document defines the cross-domain compatibility tuple used for cache artifacts and manifests.

## Compatibility schema

- **Global fallback schema version**: `rpcs3::cache::emu_cache_schema_version` (`1` currently), used only as a fallback for unknown domains.
- **Domain schema versions**:
  - `rpcs3::cache::ppu_cache_schema_version` (`1` currently)
  - `rpcs3::cache::spu_cache_schema_version` (`1` currently)
  - `rpcs3::cache::rsx_cache_schema_version` (`1` currently)
- Compatibility tuple format:
  - `schema=<N>|domain=<ppu|spu|rsx>|backend=<compiler/backend id>|platform=<platform fingerprint>`
- The tuple is produced by `rpcs3::cache::make_compatibility_tuple(...)` and stored in manifest records where applicable.

- `rpcs3::cache::make_compatibility_tuple(...)` now selects `schema=<N>` by `domain` (`ppu`/`spu`/`rsx`), so bumps can invalidate only the affected cache domain.
- Unknown domains keep using the global fallback schema constant to preserve existing behavior for non-migrated users.

### Domain-specific schema evolution policy

- Bump only the domain constant corresponding to the artifacts whose compatibility contract changed.
- Do not bump unrelated domains for isolated serializer/key changes.
- Keep existing manifest fallback checks (older `format_version` / legacy artifact names) so rolling upgrades can still consume safe older entries.
- Prefer additive evolution first (new manifest versions + fallback readers), then remove fallback once old entries are no longer expected in the field.

## Manifest record schema

Manifest lines are pipe-separated:

- Legacy format: `<artifact_type>|<hash>|<metadata>|<compatibility_tuple>|<format_version>`
- Extended format: `<artifact_type>|<hash>|<metadata>|<compatibility_tuple>|<format_version>|codec=<id>|tier=<id>`

`codec` and `tier` are optional for backward compatibility, but are required for the latest per-artifact manifest versions listed below.

- `codec` ids map to `rpcs3::cache::cas_codec`: `1=none`, `2=lz4`, `3=zstd`.
- `tier` ids map to `rpcs3::cache::cas_cache_tier`: `1=hot`, `2=warm`, `3=cold`.

Writers must record the intended artifact placement policy (`tier`) and the effective storage codec (`codec`).

## Domain/backend identity

### PPU

- Domain: `ppu`
- Backend identity includes LLVM CPU target and PPU LLVM mode bits currently affecting naming:
  - `llvm-cpu=<jit cpu>-greedy=<0|1>`
- Object naming bumped from `v7-kusa-...` to `v8-kusa-...`.

### SPU

- Domain: `spu`
- Backend identity includes LLVM CPU target + precompilation mode:
  - `llvm-cpu=<jit cpu>-precomp=<0|1>`
- File naming bumped from `...-v1-...` to `...-v2-...`.

### RSX

- Domain: `rsx`
- Backend identity is renderer backend class (`opengl`, `vulkan`) passed by RSX cache owner.
- RSX now passes explicit platform fields for shader/pipeline reuse checks via `make_rsx_platform_fields(...)`:
  - `os=<platform fingerprint from get_platform_cache_id()>`
  - `renderer=<opengl|vulkan>`
  - OpenGL: `vendor=<GL_VENDOR>`, `device=<GL_RENDERER>`, `driver=<GL_VERSION>`
  - Vulkan: `vendor_id=<VkPhysicalDeviceProperties::vendorID>`, `device_id=<...::deviceID>`, `gpu=<deviceName>`, `driver=<decoded Vulkan driver version>`
- Pipeline data embeds a compatibility hash computed from the tuple to gate pipeline binary reuse.

## Platform/driver-sensitive fields

- Default platform fingerprint (`get_platform_cache_id`) includes:
  - OS type and architecture
  - OS major/minor/patch version
  - CPU family/model
- RSX platform field formatting is deterministic:
  - Ordered `key=value` pairs joined by `;`
  - RSX tuple generation appends `os=...` then `renderer=...` in a fixed order after backend-specific fields.
- RSX explicitly includes backend-specific driver identity fields so cached pipelines are invalidated across materially different driver/GPU stacks.

## Artifact serialization versions

- **SPU cache blob manifest entries**: `spu-bin-v3` (fallback accepted: `spu-bin-v2`)
- **PPU object manifest entries**: `ppu-obj-v2`
- **RSX raw program index entries**: `rsx-raw-program-v2` (fallback accepted: `rsx-raw-program-v1`)
- **RSX pipeline binary payload**: `serialization_version = 1` in `pipeline_data` + versioned path prefix (`v1.95` today)

## Manifest validation gate policy

Before reusing manifest entries:

1. Parse manifest entry to structured fields.
2. Check artifact type.
3. Check compatibility tuple equality.
4. Check per-artifact serialization/format version.
5. For new-format records, check `codec` and `tier` match expected policy.
6. Only then fetch blob from CAS.

Compatibility behavior:

- New manifest versions require codec/tier to match.
- Older versions are still accepted through explicit fallback checks to avoid cache invalidation storms during rolling upgrades.
- CAS blobs stay content-addressed, so mixed old/new manifests can safely coexist.


## Authoritative CAS artifact policy mapping

CAS policy selection is centralized in `rpcs3/Emu/cache_utils.*` via `cas_artifact_type` + `get_policy_for_artifact(...)`. Producers should pass only artifact identity (and optional explicit tier override), never an explicit codec.

Current mapping:

- `spu_function_blob` -> manifest artifact `spu` -> default tier `hot` -> codec inferred as `LZ4`
- `ppu_object_blob` -> manifest artifact `ppu_obj` -> default tier `hot` -> codec inferred as `LZ4`
- `rsx_raw_fp_blob` -> manifest artifact `fp` -> default tier `warm` -> codec inferred as `Zstd`
- `rsx_raw_vp_blob` -> manifest artifact `vp` -> default tier `warm` -> codec inferred as `Zstd`

Rationale:

- SPU function blobs and PPU object blobs are frequently re-read on preload/hot startup paths, so they default to `hot` for faster decode (`LZ4`).
- RSX raw FP/VP blobs are more archival/rebuild-oriented and less latency sensitive per access, so they default to `warm` (`Zstd`) for better space efficiency.

## CAS telemetry for tuning

`cache_utils` maintains lightweight CAS counters for operational tuning:

- Encode/decode counts by codec (`none`/`LZ4`/`Zstd`)
- Total compressed/uncompressed throughput bytes
- Decode failure count (decompression/checksum failures)

Stats are emitted periodically to `SYS` logs (sampled every 256 CAS encode/decode events).

## CAS retention policy

- CAS blobs remain content-addressed and are not deleted during compatibility migration.
- On compatibility mismatch, old manifest references are ignored (or replaced over time by newer entries), effectively orphaning old references while preserving blobs.
