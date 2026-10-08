# Native cosmetics — signature re-verification (1.26.60.28 arm64-v8a)

Re-downloaded and re-analyzed `libminecraftpe.so` for this task. Every offset the native cosmetics
hooks rely on was **re-verified from the fresh binary**, not inherited from older notes.

## Source

- APK: `minecraft-26-60-28.apk` from mcpedl.org (`/minecraft-pe-26-60-28-apk/`, `file_id 7572`).
  382,360,914 bytes.
- `lib/arm64-v8a/libminecraftpe.so`: 368,150,880 bytes inflated.

## What was measured

| Target | Expected | Measured | Verdict |
|---|---|---|---|
| `imageLoaderSig` match | exactly once @ `0x14df7ce4` | exactly once @ `0x14df7ce4` | ✔ |
| Loader ABI | sret in x8, args x0=x1=x2=x3=x4 | `mov x19,x8` / `mov x20,x0` / `mov w21,w1` / `mov x24,x2` / `mov x23,x3` / `mov w22,w4` | ✔ |
| Payload byte | `[x19, #0x10]` | `strb w21, [x19, #0x10]` (success), `strb wzr, ...` (errors) | ✔ |
| Format dispatch | `{0,1,3,4}` | `sub w8,w21,#3; cmp w8,#2; b.lo` + `cmp w21,#1; b.eq` + `cbnz w21, ...` | ✔ |
| SIMD head store | `str q0, [out]` | `str q0, [x20]` | ✔ |
| `SerializedSkinRef::getImageData` | `+0x78` | constant-folded `add x0,x0,#0x78; ret` | ✔ |
| `SerializedSkinRef::getCapeImageData` | `+0xa8` | constant-folded `add x0,x0,#0xa8; ret` | ✔ |
| `SerializedSkinRef::getAnimatedImageData` | `+0xd8` | constant-folded `add x0,x0,#0xd8; ret` | ✔ |
| `LivePlayerRenderer` slot 17 | `0xaeaefb4` | `0xaeaefb4` (RTTI-resolved) | ✔ |
| `ClientNetworkHandler` slot 40 | `0xb80f518` | `0xb80f518` (RTTI-resolved) | ✔ |

## The relocation gotcha (read this before scanning `.data.rel.ro`)

The `Vtable.cpp` RTTI lookup works at runtime because the loader **applies** the `R_AARCH64_RELATIVE`
relocations: the typeinfo-name pointer and the vtable pointer are stored as **addends**, and the file
bytes read as `0`. A static analysis that scans the section's raw bytes for `typeName` finds
nothing and falsely concludes "RTTI is absent" (the exact trap AGENTS.md warns about). Static
verification must reconstruct relocations: build an index of `.rela.dyn` (r_offset → addend), resolve
each `.data.rel.ro` slot through it, then run the `findTypeInfo`/`findPrimaryVtableSlot` scan. That
produces exactly the same `0xaeaefb4` / `0xb80f518` the runtime `resolveVtableFunction` returns.

## Honest boundary

- The engine's `mce::Image` **internal field layout is still not recoverable** (the type exports no
  methods/symbols; accessors only reveal size). The cape swap therefore copies the whole 0x30-byte
  struct produced by the engine's own loader (`SwapCapeImage` / `BuildImageFromPng`), never a guess
  at the buffer pointer.
- A live `Actor*` for *other* players, and the player bone matrices, are still unreachable (game
  classes export no symbols); the native hook remains a passthrough that publishes ticks, and the
  resource-pack / struct-swap / image-pipeline routes are what actually carry cosmetics on shipped
  builds. The image pipeline is the one seam where a substitution into engine-owned memory is
  possible at upload time, and it is now bound to the launcher.