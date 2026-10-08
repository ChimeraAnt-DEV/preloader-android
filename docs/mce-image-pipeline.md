# `mce::Image` pipeline — the cape memory-corruption fix

## The problem

Substituting a cape by handing RenderDragon a launcher-built pixel buffer crashed or produced a
black box. Two causes, both real:

1. **`mce::Image` is an opaque handle, not a pixel array.** It owns a backing buffer that the engine
   frees through its own allocator. A pointer into the launcher's `malloc`/`std::vector` is not a
   block that allocator knows about, so the engine's free faults, and hand-filling the struct's
   internal fields leaves the buffer metadata wrong — the black-texture symptom.
2. **The loader was called with the wrong ABI.** `mce::ImageUtils::loadImageFromMemory` returns its
   `brstd::expected` aggregate through the pointer in **`x8`** (indirect return), and its arguments
   are `x0 = out image`, `x1 = ImageFormat`, `x2 = data`, `x3 = size`, `x4 = bool`. The previous
   declaration `void(*)(void* sret, void* out, u32 format, const u8* data, size_t size, bool)` mapped
   `x0 = sret, x1 = out, x2 = format, ...` and never supplied `x8`, so the out-struct was passed
   where the format belongs and the return pointer was never set.

## The evidence (1.26.60.28, arm64-v8a)

The `imageLoaderSig` rule resolves to **`0x14df7ce4`**, and the pattern matches **exactly once** in
the 368,150,880-byte `libminecraftpe.so`. Disassembling from there:

```
14df7ce4: sub  sp, sp, #0x110
14df7ce8: stp  x29, x30, [sp, #0xb0]
...
14df7d08: mov  x19, x8          ; <-- return aggregate pointer (sret), x8
14df7d0c: mov  x20, x0          ; out image struct
14df7d18: mov  w22, w4          ; bool flag
14df7d1c: mov  x23, x3          ; size
14df7d20: mov  x24, x2          ; data
14df7d24: mov  w21, w1          ; ImageFormat
...
14df7e44: strb w21, [x19, #0x10]  ; success payload byte at sret + 0x10
14df7e4c: cmp  w21, #0x4          ; format dispatch: 4 (RGBA8) accepted
14df7e50: b.eq ...
14df7e54: cmp  w21, #0x3          ; 3 accepted
14df7e5c: cmp  w21, #0x1          ; 1 accepted
```

- **`mov x19, x8`** is the sret convention: the caller supplies the return buffer pointer in `x8`
  and the function returns `void`.
- **`[x19, #0x10]`** is the `expected` payload byte: `1` on success, `0` on every error path
  (`strb wzr, [x19, #0x10]`).
- The dispatch accepts formats **`{0, 1, 3, 4}`**. `0` is auto-detect, which runs `stb_image`
  (`renoir::ThirdParty::stbi_*` symbols are present in the binary) and therefore decodes PNG. `4` is
  RGBA8 (a memcpy).

## The fix

Declare the return type as an aggregate larger than 16 bytes, so the compiler emits the `x8` sret ABI
itself:

```cpp
struct LoaderReturn { std::uint8_t storage[48]; };
using LoadImageFn = LoaderReturn (*)(void *out, std::uint32_t format,
                                     const std::uint8_t *data, std::size_t size, bool flag);
```

The generated code for `BuildImageFromPng` is the proof (from the built object):

```
add  x8, sp, #0x8      ; sret pointer
mov  x0, x2            ; out image
mov  w1, wzr           ; format = 0 (auto/PNG)
mov  x3, x19           ; size
mov  w4, wzr           ; flag = false
mov  x2, x20           ; data
blr  x9
ldrb w8, [sp, #0x18]   ; read the payload byte at sret + 0x10
```

No inline assembly is needed, and none is used: the type is the whole fix, which is what keeps it
correct across NDK versions.

## Why the detour is safe to install

`InitMceImageHook` builds a real 1×1 PNG through the loader **before** installing anything. Success
proves the address is the loader *and* the ABI is right. Only then is the detour installed; the
detour forwards the same `x8` return pointer to the original, so it is transparent. If the probe
fails, the module refuses the detour and keeps the already-proven `SerializedSkinRef` struct-swap at
`+0xa8` — the feature does not regress, it just uses the other seam.

## Substitution key

The loader receives only raw bytes, so there is no player/texture id at this seam. Overrides are
keyed by the FNV-1a hash of the **incoming** bytes (`SetContentSubstitution`), which is stable
because the engine loads the same source texture bytes on every load. `ArmNextImageOverride` is the
one-shot for an upload the caller triggers itself.

## Verified against

`libminecraftpe.so` from `minecraft-26-60-28.apk` (mcpedl.org `file_id` 7572, arm64-v8a,
368,150,880 bytes inflated; `imageLoaderSig` matched once at `0x14df7ce4`).

## The launcher surface (bound in `GameCosmeticsJni.cpp`)

The native pipeline above is driven from Java through `PreloaderInput`, so the launcher can hand the
engine a custom cape *at the moment the engine uploads a texture*. The JNI surface is:

- `nativeSetContentSubstitution(sourceBytes, rgba, w, h)` — register a rule "when the engine loads
  these exact bytes, build the image from this RGBA instead" (FNV-1a keyed on the incoming bytes).
- `nativeArmNextImageOverride(rgba, w, h)` / `nativeClearNextImageOverride` — one-shot override for
  an upload the launcher itself triggers.
- `nativeSubstitutionCount` / `nativeClearContentSubstitutions`.
- `nativeIsMceImageHookLive` / `nativeIsImagePathVerified` / `nativeReadMceImageHookStats` —
  diagnostics the UI reads to report which route is actually live.
- `nativeIsTextureCacheFlushAvailable` / `nativeRequestTextureCacheFlush`.

Every call is fail-closed. On a build where the loader probe failed, the calls are no-ops that
report "not installed", and the pack path (or the `SerializedSkinRef` struct-swap) keeps carrying the
cosmetic. See `NativeCosmeticsBridge.registerSubstitution` /
`NativeCosmeticsFeed.describeImagePipeline` for the launcher-half.

## Verified by re-analysis (minecraft-26-60-28.apk, file_id 7572)

Re-downloading the APK and re-running the disassembly confirmed every number this doc relies on:

- `imageLoaderSig` matches **exactly once** at `0x14df7ce4`.
- The loader ABI is exactly as recovered: `mov x19, x8` (sret), `mov x20, x0` (out), `mov w21, w1`
  (format), `mov x24, x2` (data), `mov x23, x3` (size), `mov w22, w4` (flag), payload byte written
  at `[x19, #0x10]`, SIMD `str q0, [x20]` head store, and the format dispatch `{0, 1, 3, 4}`.
- `LoadImageFn` (the `x8` sret return-type declaration) is the correct ABI; the built object
  emits `add x8, sp, #0x8` before the `blr`.
- The `SerializedSkinRef` accessor offsets (`getImageData +0x78`, `getCapeImageData +0xa8`,
  `getAnimatedImageData +0xd8`) are present as constant-folded `add x0,x0,#imm; ret` accessor
  bodies — `mce::Image` is exactly 0x30 bytes.
- `LivePlayerRenderer` vtable slot 17 resolves (via the relocation-aware `Vtable.cpp` replica) to
  `0xaeaefb4` — the same render entry the player-render feed and the native cosmetics hook use.
- `ClientNetworkHandler` vtable slot 40 resolves to `0xb80f518`.

The `.data.rel.ro` pointers are RELATIVE-relocation addends (`R_AARCH64_RELATIVE`); a static byte
scan of the section reads 0, so the typeinfo lookup must be relocation-aware (which `Vtable.cpp`
is at runtime, where relocations are applied).

## Still not implemented (honest boundary)

The **pixel substitution target** at this seam — rewriting `data`/`size` for a specific skin — is
implemented, but the **texture-cache flush** is not fabricated: `mce::TextureGroup` / `SkinRepository`
are not RTTI-resolvable in the stripped binary, so `RequestTextureCacheFlush` invokes a
caller-proven callback and reports false when none exists. Reaching the flush needs a verified vtable
entry, which is a separate disassembler pass.
