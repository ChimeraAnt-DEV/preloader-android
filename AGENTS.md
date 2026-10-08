# AGENTS.md

## Build
- Android-only CMake target (`preloader`), C++20, NDK toolchain. `CMakeLists.txt`
  fails fast off-Android and on any ABI other than `arm64-v8a` / `armeabi-v7a`.
  GlossHook static lib is required at `lib/ARM64/libGlossHook.a` (arm64) or
  `lib/ARM/libGlossHook.a` (armv7).
- Sources are listed explicitly in `PRELOADER_SOURCES`; **a new `.cpp` must be
  added there** or it silently will not build into the library.

## JNI symbol naming
- JNI exports bind to the **`org.chimeramc.client.*`** package
  (`Java_org_chimeramc_client_*`). The launcher's `PreloaderInput` lives at
  `org.chimeramc.client.preloader.PreloaderInput` (its source directory is
  `.../launcher/preloader/`, but the package is `org.chimeramc.client.preloader`).
  Verify a build with
  `llvm-nm -D --defined-only <built libpreloader.so> | grep Java_org_chimeramc_client`.

## Pattern scanning / hooks
- `pl::memory::resolveSignature(s)` scans a module's readable `/proc/self/maps`
  regions for byte signatures; `resolveVtableFunction("14TypeName", slot, module)`
  resolves an RTTI-named vtable slot through `.rodata`/`.data.rel.ro`; `pl::memory::hook`
  installs a detour. All are used by `GameHooks.cpp` / `GameLocalPlayer.cpp`.
- Per-version byte signatures and offsets come from `GameHookRules` JSON, configured
  from the launcher via `nativeConfigureSignatureRules(rulesPath, version)`. Keep new
  rules there rather than hardcoding a build list in C++.

## Local-player feed (`GameLocalPlayer`)
- Hooks `ClientInstance` vtable slot 31 (resolved by RTTI name) to capture the live
  local player once per frame; publishes a seqlock-protected snapshot read by Java.
  Fail-closed: no hook / no world reads as "no data". Field offsets `Actor+0x230`
  (position) and `Actor+0x238` (rotation) are shared across the targeted builds.

## Player-render hook (`GamePlayerRender`)
- **Hook point: `LivePlayerRenderer::render`, primary-vtable slot 17**, resolved by RTTI name
  (`resolveVtableFunction("18LivePlayerRenderer", 17, "libminecraftpe.so")`). No per-build code
  address is baked in; the slot index is overridable from the signature rules
  (`playerRenderVtableIndex`), default 17. See `docs/player-render-hook.md` for the full finding
  and the identification method (RTTI name → vtable dump → per-slot string scan: slot 17 is the
  only slot referencing `variable.player_x_rotation` / `is_first_person` / `is_using_vr`).
- **The detour is a pure passthrough.** The game strips symbols, so the render entry's C++
  signature is not recoverable; the hook forwards `x0..x7` unchanged and dereferences nothing.
  That is what makes an unknown ABI safe — a wrong signature cannot read freed memory or corrupt
  a return value because no argument is ever touched.
- **What it provides:** `IsPlayerRenderHookLive()` and `ReadPlayerRenderStats()` →
  `{renderTick, callsThisFrame, totalCalls, msSinceLastRender}`. The renderer runs once per
  rendered player, so `callsThisFrame > 1` is direct evidence it covers non-local players too.
  The render tick is the per-frame clock the launcher's native cape/pet physics advance on.
- **Fail-closed:** unresolved slot / failed install / renderer never ran → the launcher keeps the
  resource-pack path. Never crashes, never partially renders. Runtime-detected, no version
  allowlist.

## Engine image pipeline (`mce_image_hook`) — the cape memory-corruption fix
- **`mce::Image` is opaque; build it through the engine, never by hand.** It is a 0x30-byte handle
  (derived from `SerializedSkinRef`: base skin at `+0x78`, cape at `+0xa8`, exactly 0x30 apart) whose
  backing buffer RenderDragon frees with its own allocator. A launcher-side `malloc`/`std::vector`
  handed to it faults on free or double-frees, and hand-filling the struct gives the black-box
  texture because the internal buffer metadata is wrong. `mce_image_hook.cpp` is the only place the
  engine loader is called; everything goes through `BuildImageFromPng`/`BuildImageFromRgba`.
- **The loader's ABI is sret-in-`x8`, and the previous code got it wrong.** Disassembly of
  `mce::ImageUtils::loadImageFromMemory` at the `imageLoaderSig` address (`0x14df7ce4` in
  1.26.60.28; the pattern matches exactly once) shows `mov x19, x8` in the prologue and the success
  flag written at `[x19, #0x10]` — the AArch64 indirect-return convention with the return pointer
  in `x8`. The real argument mapping is `x0 = out image`, `x1 = ImageFormat`, `x2 = data`,
  `x3 = size`, `x4 = bool`. The old `using LoadImageFn = void(*)(void* sret, void* out, u32, const
  u8*, size_t, bool)` mapped `x0=sret, x1=out, x2=format, ...` and never supplied `x8`, i.e. it
  passed the out-struct where the format belongs — the source of the corruption. **The fix is a type
  fix:** declare the return type as a struct larger than 16 bytes (`LoaderReturn`) and Clang emits
  the `x8` sret ABI itself; the generated code (`add x8, sp, #0x8` before the `blr`, flag read from
  `sret+0x10`) is the proof. Do not go back to a hand-written `void(...)` signature.
- **ImageFormat values the loader accepts are `{0, 1, 3, 4}`** (read from the dispatch in the same
  routine: `cmp w21, #1`, `sub w8, w21, #3; cmp w8, #2`). `0` is auto-detect → `stb_image`, which
  decodes PNG (the cape texture route); `4` is RGBA8 (a memcpy). `renoir::ThirdParty::stbi_*` is
  present in the binary, confirming the PNG path.
- **Installation is gated on a live probe, not a version list.** `InitMceImageHook` runs a one-shot
  probe: build a real 1×1 PNG through the loader. Success proves both the address and the ABI;
  failure refuses the detour and keeps the already-proven `SwapCapeImage` struct-swap. This is the
  runtime-detected, never-allowlisted rule the other seams follow.
- **The loader seam has no id — key substitutions by content hash.** `loadImageFromMemory` receives
  only raw bytes, so `SetContentSubstitution(sourceBytes -> replacementRgba)` keys on the FNV-1a of
  the *incoming* bytes (stable across loads), and `ArmNextImageOverride` is the one-shot for a known
  upload the caller triggers. The detour rewrites `data`/`size`/`format` and calls the original with
  the same sret pointer, so it stays transparent.
- **Texture cache flush is a proven-callback seam, not a fabricated one.** `mce::TextureGroup` /
  `SkinRepository` are not RTTI-resolvable in the stripped binary, so `RequestTextureCacheFlush`
  invokes a caller-installed `TextureFlushFn` and honestly returns false when none is proven (the
  caller then treats the change as next-bind, not live).
- **`EngineAllocate`/`EngineFree` refuse foreign and double frees** (tracked set + stored alignment
  so the aligned `operator delete` matches its `operator new`). Prefer the engine's own allocation
  via `BuildImageFrom*`; use these only when a buffer must exist before the engine call.

## Live resource-pack reload (`GameResourcePackReload`)
- `nativeReloadResourcePacks()` / `pl::runtime::ReloadResourcePacks()` is a **fail-safe
  research seam**, not a working live reload. Reverse-engineering of
  `libminecraftpe.so` (1.26.50.4 / 1.26.60.28) found the pack machinery
  (`ContentManager::reloadSources`, `ResourcePackManager::setStack`/`_doStackOperation`,
  `RepositoryLoading::refreshPacks`/`reloadUserPacks`, `ReloadCommand`,
  `TextureHotReloader`) but **no safe, verifiable in-place refresh**: `reloadSources`
  throws unless its async init task completed (i.e. not during play), the stack
  operations are reached only from world setup/teardown, and game classes export no
  symbols so there is no hook anchor. The export therefore resolves to "no hook",
  returns false, and the launcher falls back to relaunching the instance.
- **Runtime detection, never a version allowlist.** A confirmed refresh (when a
  verified hook lands) marks the running build supported in memory; an unsupported
  build or a failed call is never retried in the same session.
- See `docs/resource-pack-reload.md` for the full finding, the technique
  (strings -> `adrp`/`add` xrefs -> `.eh_frame_hdr` function bounds) and the
  fragility note (a Minecraft update re-verifies from scratch; no list to maintain).
