# Native cosmetic render route — raw GLES vs the engine's own geometry

This note records the research for Phase D: instead of rasterising cosmetics with raw GLES from the
player-render hook, could the launcher attach the cosmetic geometry to the engine's own model and let
the engine draw it?

## Where the two routes stand

**Raw GLES (current).** `pl::cosmetics::NativeCosmeticRenderer` draws the cape chain, hat and pet as
its own meshes inside the hooked `LivePlayerRenderer::render` pass, using a runtime-resolved GLES
function table (never linked, so a Vulkan-only RenderDragon build leaves it null and fails closed).
This works but has three known limits, all of which the fix roll-up in AGENTS.md addresses as far as
is possible without the engine route:

1. It is a second draw pass, so it must save/restore GL state (and the state-query bug — wrong
   pnames — was what corrupted the game's program/VAO).
2. Placement depends on a recovered view-projection matrix (`pl::render::CameraSource`) or a
   first-person fallback (`ComputeViewProjection`); the hook arguments are an unknown ABI, so the
   real per-model transform is not readable.
3. It cannot attach a cosmetic to another player's model without re-deriving that model's pose.

**Engine geometry (research target).** The engine already draws the player model from its own
geometry (bones, `.geo.json` in a resource pack or the client entity file). If a cosmetic could be
added as extra bones to *that* model, the engine would pose, transform and draw it with the player —
no second pass, no state save/restore, no separate matrix.

## Why the engine route is not reachable from this build

- **Game classes export no symbols.** The shipped `libminecraftpe.so` has no `_ZN...` game symbols,
  so there is no `GeometryPtr`/`Model`/`Skeleton` accessor to call, and no vtable slot whose meaning
  is confirmed for a "add a bone / register geometry" operation. The RTTI names that *do* resolve
  (`LivePlayerRenderer`, `ClientInstance`, `ClientNetworkHandler`) give render entries and accessors,
  not a mutable geometry registry.
- **The render entry's arguments are opaque.** `render` is a passthrough detour (`x0..x7` forwarded,
  nothing dereferenced), so even the `this` renderer pointer is not safely usable to reach its model
  without knowing the object layout, which the stripped binary does not expose.
- **A resource pack is the supported "engine geometry" path**, and it is exactly the route this
  project moved away from: a pack cannot carry a client cosmetic (the engine strips custom entity
  animations / bone retargeting outside a registered `animation_controller` in a world context), it
  only applies on a world reload, and it is visible only to players who also run the pack.

## Consequence

The honest deliverable remains the native hook: it establishes the frame tick, resolves the slot by
RTTI name across builds, and drives the cosmetic meshes. Adding bones to the engine's own geometry
would need a disassembler pass to recover the engine's model/geometry object layout on each build —
a per-build research project, not a one-off change — so it is **not** in this change.

## What IS reachable and shipped

- The render slot is resolved at runtime (by the diagnostic string its body references, then the
  per-version rule, then the default), so the hook works across builds instead of one.
- The persistent-buffer draw path, per-piece textures, the ÷16 scale, the Bedrock→player orientation
  conversion and the real-camera/pose anchor, so the meshes appear at the right place and size.
- `CosmeticFrame` (Java → native) drives the per-segment cape lean/sway, the hat head-look and the
  pet per-bone pose from the game's own frame tick.

If a future build's `LivePlayerRenderer` model object layout is recovered, a `BoneMatrixSource`
(already declared in `NativeRenderHook.hpp`) can feed real bone matrices in place of the
`world * offset` anchors without changing the draw code.
