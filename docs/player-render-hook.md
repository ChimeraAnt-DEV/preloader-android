# Player-render hook — research finding and pattern

## Goal

The cosmetics system renders capes/hats/pets through a resource pack. A pack cannot move
cloth per-bone, cannot apply without a world reload, and cannot put a cosmetic on another
player's model unless that player also has the pack. The native path hooks the game's own
player renderer so the launcher can drive the cosmetic motion from the transform the game
actually renders the player with, and so the same hook runs for non-local players.

## Hook point

**`LivePlayerRenderer::render`**, reached by RTTI name
`resolveVtableFunction("18LivePlayerRenderer", slot, "libminecraftpe.so")` — the same
name-resolution mechanism the local-player feed uses for `ClientInstance` slot 31. No
per-build code address is baked in.

### The slot index moves between builds — resolve it at runtime, do not hardcode it

The primary-vtable slot for `render` is **not stable across builds**: it was verified as slot
**17** on the 1.26.60.x binaries, and on **26.52** slot 17 is null and the render diagnostic is
not referenced by a `LivePlayerRenderer` vtable slot at all. A hardcoded index is therefore
correct for exactly one build.

The hook resolves the slot at runtime in two steps:

1. `pl::memory::resolveVtableSlotReferencingString("18LivePlayerRenderer",
   "variable.player_x_rotation", ...)` — finds the slot whose function body references the
   render diagnostic via an `adrp`/`add` pair, which needs no per-build index.
2. If that finds nothing, fall back to the per-version `playerRenderVtableIndex` rule, then the
   built-in default (17).

Before any detour is installed, `pl::memory::isExecutableAddress()` confirms the resolved
address lies in an executable mapping — a stale index that lands in data is refused rather than
hooked. The install also only runs on builds at or above the floor version (1.21.132); newer
builds keep the attempt because the guard, not a version allowlist, is what makes it safe.

### How it was identified (method: RTTI name → vtable → string scan)

1. `18LivePlayerRenderer` is a standalone RTTI name string in `.rodata`, so it resolves like
   any other typeinfo name (the in-repo `resolveVtableFunction` finds its typeinfo in
   `.data.rel.ro` via the `R_AARCH64_RELATIVE` relocation, then reads the primary vtable).
2. Dumping the vtable and scanning each slot function's `adrp`/`add` string references
   isolates the render entry: on 1.26.60.x **slot 17** is the only slot whose body references
   `variable.player_x_rotation`, `variable.is_first_person` and `variable.is_using_vr` — the
   Molang/pose variables a player-model render routine sets.
3. On 26.52 the diagnostic is referenced by functions that are **not** `LivePlayerRenderer`
   vtable slots, so the string scan declines there and the configured index is used; a build
   whose index is also wrong fails closed (the executable-address guard refuses the hook).

### Why the entry is safe to detour without knowing its ABI

The game strips its symbols, so the C++ signature of `render` cannot be recovered statically.
The detour is therefore a **pure passthrough**: it forwards the full integer argument register
set (`x0..x7`) unchanged and dereferences **nothing**. That is what makes an unknown ABI safe
— a wrong signature cannot read freed memory, mis-handle `this`, or corrupt a return value,
because the hook never touches an argument. It only records counters/timestamps and calls the
original trampoline.

## What the hook provides

`pl::runtime::GamePlayerRender` exposes, over JNI:

- `nativeIsPlayerRenderHookLive()` — the renderer has actually run this session.
- `nativeReadPlayerRenderStats()` — `{renderTick, callsThisFrame, totalCalls, msSinceLastRender}`.

The renderer runs once per rendered player, so `callsThisFrame > 1` is direct evidence the
hook covers non-local players as well as the local one. The render tick is the honest frame
clock the native cape/pet physics advance on (the launcher has no other per-frame signal).

## Fail-safe contract

- Slot unresolved, hook install failed, or the renderer never ran → `IsPlayerRenderHookLive()`
  is false and the launcher keeps the resource-pack path.
- The detour never dereferences, allocates, or locks, so it cannot fault or stall the render
  path; if the hook cannot be installed the game runs exactly as before.
- Runtime-detected: the slot index is read from the per-version signature rules
  (`playerRenderVtableIndex`) with the confirmed default (17) as a fallback; there is no
  version allowlist.

## Fragility note

A Minecraft update re-verifies the slot from scratch. The RTTI name resolution means the
lookup itself is version-stable, but the slot index is not (renderer vtables are reordered
between builds), so `playerRenderVtableIndex` belongs in the rules JSON next to the other
per-version values. If it drifts, the failure mode is "hook resolves to a different method,
the launcher sees no render ticks, and it falls back to the pack" — never a crash.

## Verified against

`libminecraftpe.so` from `minecraft-26-60-28.apk` (arm64-v8a, 368,150,880 bytes inflated),
cross-checked against the previously analysed 1.26.50.4 binary.
