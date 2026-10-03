# Live resource-pack reload — research finding

## Goal

Apply an equipped cape / hat / pet to a **running** world without making the
player leave it. Today the launcher writes the pack files and then relaunches
the instance (`MinecraftSessionRestarter`); a live refresh would remove the
relaunch.

## What was examined

`libminecraftpe.so` from `minecraft-26-60-28.apk` (arm64-v8a, 368,150,880 bytes
inflated; fetched from `file.mcpedl.org` and inflated from the deflated ZIP
entry), cross-checked against the previously analysed 1.26.50.4 binary.

Technique: `strings` for the game's own diagnostic and source-path strings,
then `adrp`+`add` cross-references into `.rodata` to find the code that
references them, then `.eh_frame_hdr` (the FDE search table) to recover exact
function start/end addresses. No absolute offsets are used in the shipped code;
this was analysis only.

## Refresh entry points that exist

| Symbol / source | Found at | Why it is not safe on a live session |
| --- | --- | --- |
| `ContentManager::reloadSources(bool)` (`SourcesAsyncReloader.cpp`) | xref of `"mReloadSourcesAsync->isInitTaskCompleted()"` | Guarded: it **throws** when the async init task has not completed, which is exactly the state during play. |
| `ResourcePackManager::setStack(...)` / `_doStackOperation(...)` / `getStack(...)` | xrefs of `"Invalid value passed to ResourcePackManager::_doStackOperation"` | `_doStackOperation`/`getStack` are const accessors with no visible trigger; `setStack` is reached only from world setup/teardown. |
| `RepositoryLoading::refreshPacks` / `reloadUserPacks` (`ResourcePackRepositoryRefreshQueue.cpp`) | string table | Repository-level refresh; invoked by the world load/teardown flow, not a public live trigger. |
| `ReloadCommand` (`ReloadCommand.cpp`) | xref of its source path | `/reload` re-reads JSON / script / custom-component content and explicitly tells the player to exit and rejoin for new components. It does **not** re-scan the resource-pack stack. |
| `TextureHotReloader`, `MinecraftGame::_hotReloadTextureAtlases` | `TextureHotReloader.cpp` | Gated behind `enable_texture_hot_reloader` (a debug option) and restricted to texture atlases: "Hot reloader doesn't work with .mcpack or .zip resource packs". Not a general pack refresh. |

## Why no hook was installed

* **Game classes export no symbols.** Every `FUNC` in `.dynsym` is third-party
  (v8 / cohtml / renoir / webrtc / XAL / leveldb). There is no
  `_ZN...ResourcePackManager...` or `...ContentManager...` symbol, so `dlsym`
  cannot reach them and there is no stable anchor to hook.
* **Calling a C++ method from outside needs an ABI contract** (correct `this`,
  argument types, side effects, and threading). None of these can be verified
  without a device, and calling `reloadSources` during play is a documented
  throw path.
* **Every reachable refresh routes through world setup/teardown.** The
  conclusion matches the escape clause in the task: the relaunch path is the
  correct route for the shipped builds.

## What is shipped instead

`pl::runtime::ReloadResourcePacks()` (`GameResourcePackReload.cpp`) plus the
JNI export `Java_org_chimeramc_client_preloader_PreloaderInput_nativeReloadResourcePacks`:

* resolves a refresh hook **once per process** (currently none for the shipped
  builds — see above);
* returns `false` immediately when there is no verified hook, so the launcher
  relaunches;
* **runtime detection, no hardcoded version list**: the first confirmed refresh
  marks the running build supported *in memory* (`IsResourcePackReloadSupported`);
  an unsupported build or a failed call is **never retried in the same session**;
* fail-safe: never throws, never partially applies.

When a per-build signature for a genuinely in-place refresh is derived and
verified on device, `ResolveRefreshHook` stores the address and
`ReloadResourcePacks` invokes it and checks the engine's own confirmation
(the active pack re-read, or a reload counter it exposes) before reporting
success. Only that confirmed path sets the supported flag.

## Fragility note

Per-build detection means a Minecraft update re-verifies from scratch. There is
no version allowlist to maintain, and an unverified build simply falls back to
relaunch.
