# RPCS3 Shared Cache Key Schema (PPU/SPU/RSX)

This document defines the cross-domain compatibility tuple used for cache artifacts and manifests.

## Global schema

- **Emulator cache schema version**: `rpcs3::cache::emu_cache_schema_version` (`1` currently).
- Compatibility tuple format:
  - `schema=<N>|domain=<ppu|spu|rsx>|backend=<compiler/backend id>|platform=<platform fingerprint>`
- The tuple is produced by `rpcs3::cache::make_compatibility_tuple(...)` and stored in manifest records where applicable.

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
- Pipeline data embeds a compatibility hash computed from the tuple to gate pipeline binary reuse.

## Platform/driver-sensitive fields

- Platform fingerprint currently includes:
  - OS type and architecture
  - OS major/minor/patch version
  - CPU family/model
- For RSX, backend identity should be extended in future with explicit driver identity fields when available (e.g. Vulkan driver build, GL vendor/renderer string) to tighten shader pipeline portability.

## Artifact serialization versions

- **SPU cache blob**: `spu-bin-v2`
- **PPU object manifest entries**: `ppu-obj-v1`
- **RSX raw program index entries**: `rsx-raw-program-v1`
- **RSX pipeline binary payload**: `serialization_version = 1` in `pipeline_data` + versioned path prefix (`v1.95` today)

## Manifest validation gate policy

Before reusing manifest entries:

1. Parse manifest entry to structured fields.
2. Check artifact type.
3. Check compatibility tuple equality.
4. Check per-artifact serialization/format version.
5. Only then fetch blob from CAS.

Mismatched entries are rejected.

## CAS retention policy

- CAS blobs remain content-addressed and are not deleted during compatibility migration.
- On compatibility mismatch, old manifest references are ignored (or replaced over time by newer entries), effectively orphaning old references while preserving blobs.
