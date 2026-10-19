# Preloader item status (the 22-item list)

This is the honest state of each item in the 22-item preloader list. An item is **done** only when
the mechanism exists and is testable without a device; **partial** means the safe half is real and
the rest needs a per-build recovery that has not been done; **blocked** means the build exposes
nothing to read and the code says so rather than guessing.

The distinction matters because most of these items are gated on a *recovery* step — reading a field
layout out of the stripped `libminecraftpe.so`. Where that recovery has not happened, the code is a
verified passthrough or a fail-closed refusal, never a fabricated reading.

| # | Item | State | Where | Note |
| --- | --- | --- | --- | --- |
| 1 | Type each detour with its real return value | done | `RegisterAwareHook.cpp` | Return-transparent veneers preserve x0, x8 (sret) and q0; a typed `void` detour is gone. |
| 2 | Persona skin capture | blocked | `tools/verify-binary` (`persona_names_present`) | The analyzer reports which persona accessor/type names a build carries. No build checked here exposes them, so no persona layout is claimed. |
| 3 | Remote skin diagnostics | partial | `SkinSeamObservation.hpp` | The join seam is classified (absent / idle / no image / with image) from reach counters; the payload's skin fields are not readable, and the report says so. |
| 4 | Retry the hook install | done | `HookInstallGate.hpp` | Re-arms after a deferral instead of giving up permanently; pinned by `HookInstallGateTest`. |
| 5 | Retry rule delivery | done | `GameHooks.cpp` | A late rules file installs the hooks when it arrives; the install is deferred, not skipped. |
| 6 | Real version gate | done | `GamePlayerRender.cpp` | A live probe (slot body references the render diagnostic, address executable) installs on any build; the `1.21.132` floor guards only the weaker configured-index fallback. |
| 7 | Verify patched prologues | done | `Vtable.cpp`, `PrologueVerify.hpp` | The first instructions are read back after install; an unverified patch is backed out. |
| 8 | Real entity culling | blocked | `OptifineEntityCulling.cpp` | Verified passthrough: resolves the entry and counts calls. Skipping off-screen entities needs the actor transform + frustum, which this build does not expose. |
| 9 | Real particle culling | blocked | `OptifineParticleCulling.cpp` | Same shape as item 8 for `ParticleEngine`. |
| 10 | Real dynamic render distance | partial | `OptifineRenderDistance.cpp` | The options setter must be found from the option's string references; until then the governor's output is reported, not applied. |
| 11 | Frame-time-based pacing | partial | `FramePacing.hpp` | The frame intervals are measured and classified (jitter, vsync-lock) and pinned by `FramePacingTest`; actively rescheduling against the display's vsync needs the engine's own present call, which the stripped binary does not expose. The reading is real; the reschedule is not. |
| 12 | Vulkan cosmetic path | done | `mce_image_hook.cpp`, `GlesProbe.hpp` | A cosmetic reaches the game through the engine image loader, which does not need GLES, so the cape path works on a Vulkan-only backend. The mesh renderer that needed GLES is gone, so there is no GLES-only cosmetic path left to fall back from. |
| 13 | Texture-bind slot discovery | partial | `GameCosmetics.cpp` | The slot comes from the rules (`textureBindVtableIndex`); the default is 0 and the seam is fail-closed. The slot itself is not yet recovered per build. |
| 14 | Bone matrix source | partial | `NativeRenderHook.cpp` | `BoneMatrixSource` is the seam and is fail-closed with no provider; recovering the matrices is the remaining step. |
| 15 | Texture-cache flush proof | partial | `mce_image_hook.hpp` | `SetTextureCacheFlusher` is a caller-proven callback, not a fabricated one; finding a verified flush callback is the remaining step. |
| 16 | Automated rule generation | done | `tools/gen-rules` | Download → extract → analyze → merge → bump revision, with its own host tests. |
| 17 | Probe-backed rules | done | `RuleProbe.hpp`, `GameHookRuleMerge.hpp`, `tools/verify-binary` | A generated rule carries its own probe; the merge keeps it, and the gate refuses a slot whose probe did not resolve. |
| 18 | Per-gate failure reasons | done | `GamePlayerRender.cpp` | The exact refused link (version, slot, executable, hook, probe) is reported in the JNI status. |
| 19 | Crash quarantine | done | `OptifineConfig.cpp` | The existing `crash.<itemId>` counter auto-disables an item after repeated crashes and says why. |
| 20 | Config size limits | done | `ModMenuBridge.cpp` | The launcher-to-native config blob is bounded in size, key count and string length before parsing. |
| 21 | Host tests in CI | done | `.github/workflows/android.yml` | The preloader host tests and the Python tooling tests run on every commit; the tooling tests install numpy and the analyzer also works without it. |
| 22 | Honest diagnostics export | done | `CosmeticsDiagnosticsReport.java`, `GameCosmeticsJni.cpp` | One copyable report: probe results, hook install states, GPU backend, and "unknown" for anything the build cannot read. |

## The four that matter for the skin bug

Items 1, 2, 3 and 7 are the ones the skin report calls out.

- **1 (typed detours)** is done: the veneers forward the full register set, so a return value in x0
  or an sret pointer in x8 survives the detour. The old `void` detours dropped them.
- **7 (prologue verification)** is done: a detour whose patched prologue does not read back is
  backed out, so a bad install cannot leave a half-patched function.
- **2 (persona capture)** is blocked, honestly. The persona piece fields live in the stripped
  binary; the analyzer now reports which persona accessor names a build carries, and none of the
  builds checked carry them, so no layout is invented. Recovering the layout needs a disassembler
  pass on a build that does carry the names.
- **3 (remote skin diagnostics)** is partial, honestly. The join seam is classified from the reach
  counters the detours already keep. The payload's skin fields are not readable here, so the report
  names the failing step (join hook absent / idle / no image / downstream) rather than claiming the
  skin was or was not sent. The device comparison (a server that shows the skin vs one that does
  not) is documented in `docs/SKIN_JOIN_FINDING.md` and cannot run in this repo — there is no
  emulator or device.

## Why "blocked" is the right label, not "todo"

The game classes export no symbols; the RTTI seams resolve by name, but a typeinfo name yields a
vtable, not a field map. Reading an actor's transform, a persona piece list or a particle emitter
layout means disassembling a specific build. Until that happens, a hook that guessed at an object
layout would corrupt rendering rather than improve it, so these items install a verified passthrough
and label it as a measurement. That is deliberate and is the same boundary the combat modules
document.
