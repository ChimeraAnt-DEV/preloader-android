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
