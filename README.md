# preloader

A native runtime that loads into the Minecraft Bedrock process and exposes hook, signature and
input primitives to the Glowberry Client launcher and to native mods.

It is a **shared library**, not an app. The launcher's `MinecraftActivity` calls
`System.loadLibrary("preloader")`, which maps it into the game's own process — that is the point:
a mod's hooks must run in the address space of `libminecraftpe.so`.

## What it does

- **Signature resolution.** Finds a byte pattern or a vtable slot inside a stripped game binary,
  where no symbol exists. Game classes export no dynamic symbols, so this is the only route to an
  address.
- **Hooking.** Installs an in-place trampoline (`pl::memory::hook`) through the vendored GlossHook
  backend, with a prologue verification so a stale pattern cannot corrupt code.
- **Input bridging.** Receives touch, key, text and mouse events from the game and forwards them to
  registered callbacks (the launcher's `PreloaderInput`, and native mods).
- **Cosmetics rendering.** Draws the equipped cape, hat and pet on the local player through the
  game's own player renderer.
- **Mod Menu / HUD bridge.** Supplies the in-game Mod Menu and HUD overlays their state.

## Layout

```
include/pl/     public headers (the API a mod or the launcher includes)
src/pl/         implementation
  memory/       signature resolution, hooking, patching, ELF helpers
  runtime/      game hooks, cosmetics, input bridge, optifine mode
  cosmetics/    geometry, animation, network
  render/       GL probing, camera source
  hooks/        the hook-manager facade
  legacy/       the C entry points kept for older mods
lib/ARM64/      vendored prebuilt GlossHook static library
tests/          host tests (pure C++, no device needed)
docs/           per-area notes
```

## Building

The preloader is built as part of the client APK; you normally do not build it on its own. The
client's `app/build.gradle` adds `app/src/main/cpp/preloader` as a CMake subdirectory, and the
NDK toolchain compiles it for `arm64-v8a`.

To build it directly:

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=28
cmake --build build
```

`arm64-v8a` is the only supported ABI. The client ships a single arm64 APK, and GlossHook is
vendored for arm64 only; a 32-bit configure is refused rather than silently producing a library
the client would never load.

Dependencies are fetched at configure time and pinned by commit in `CMakeLists.txt` (see `NOTICE`).

## Host tests

The pure parts — the geometry parser, animation solver, socket protocol, signature pattern parser,
ELF build-id reader, callback snapshot — carry no Android types and are tested on the build host:

```sh
bash tests/cosmetics/run_tests.sh
```

They need a host C++20 compiler and `zlib` headers. The `nlohmann/json` single header is fetched
into a gitignored cache on first run, or point `NLOHMANN_JSON_INCLUDE` at an existing copy.

## Writing a mod against it

A mod is a shared library that the client's mod loader `dlopen`s. It includes the public headers
under `include/pl/` and links against the preloader, which is already resident in the process:

```cpp
#include "pl/memory/Signature.hpp"
#include "pl/memory/Hook.hpp"

static int (*originalTick)(void*) = nullptr;
static int hookTick(void* self) { /* ... */ return originalTick(self); }

void mod_init() {
  auto target = pl::memory::resolveSignature(".. ?? ..", "libminecraftpe.so");
  if (target) {
    pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(target),
                     reinterpret_cast<pl::memory::FuncPtr>(hookTick),
                     reinterpret_cast<pl::memory::FuncPtr*>(&originalTick),
                     pl::memory::HookPriority::Normal, "mod.tick");
  }
}
```

Notes that matter:

- **A signature is valid for one build only.** A game update relinks the binary and the pattern
  stops matching; check the return value and fail closed rather than hooking a wrong address. The
  resolver keys its cache by the module's ELF build id for exactly this reason.
- **`hook` is the one exported signature.** The name is a parameter, not a default argument, so the
  symbol a mod links against is the symbol the library exports.
- **Hook callbacks run on the game's own threads.** They must not block, allocate per frame, or
  take a lock that a render path also takes. See `docs/player-render-hook.md`.
- **Only `arm64-v8a` is supported**, and the game must be a build the signature rules cover.

## ABI policy

The public surface is the C++ headers under `include/pl/`, plus the C entry points in
`src/pl/legacy/`. A mod that links against the preloader is coupled to that surface: an exported
function's signature or a header's layout must not change without a version bump and a note here.
The `pl::memory::hook` signature is the clearest example — it is a parameter list, not a default
argument, so that a mod built against a different arity fails to load loudly rather than binding to
the wrong function.

## Docs

- `docs/player-render-hook.md` — the player-render seam the cosmetics renderer uses.
- `docs/mce-image-pipeline.md` — the engine image-loader seam.
- `docs/native-cosmetic-render-route.md` — why the engine-geometry route is not reachable.
- `docs/resource-pack-reload.md` — why an in-place pack reload is not implemented.
- `docs/preloader-item-status.md` — per-item status of the hardening work.

## Licence

Apache-2.0. See `LICENSE`. Third-party components and their licences are listed in `NOTICE`.
