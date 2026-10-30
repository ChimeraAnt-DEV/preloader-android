/**
 * @file LegacyAbiTest.cpp
 * @brief Host test that pins the exported C ABI surface at compile time.
 *
 * The legacy C entry points are a public ABI for mods built outside this repository. If one is
 * renamed or removed, the mod fails to load with a symbol error and nothing else in this repo
 * notices, because nothing here calls them. Naming each entry point's exact function type in a
 * `static_assert` makes a rename or an arity change a build failure instead.
 *
 * Only `decltype` is used, never a value, so the test needs the headers and not the Android-linked
 * implementations.
 *
 * The `pl::memory::hook` arity is pinned for the same reason: the exported symbol *is* the
 * parameter list, so a mod built against a different arity would resolve the wrong function or
 * none at all.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <type_traits>

#include "pl/legacy/LegacyHook.h"
#include "pl/legacy/LegacyInput.h"
#include "pl/legacy/LegacyPatch.h"
#include "pl/legacy/LegacySignature.h"
#include "pl/memory/Hook.hpp"

namespace {

using HookType = int (*)(PLFuncPtr, PLFuncPtr, PLFuncPtr *, PLHookPriority);
using UnhookType = bool (*)(PLFuncPtr, PLFuncPtr);
using GetInputType = PreloaderInput_Interface *(*)(void);
using PatchBytesType = bool (*)(uintptr_t, const uint8_t *, size_t, const char *);
using ResolveSignatureType = uintptr_t (*)(const char *, const char *);

static_assert(std::is_same_v<decltype(&pl_hook), HookType>,
              "pl_hook must keep its C hook arity");
static_assert(std::is_same_v<decltype(&pl_unhook), UnhookType>, "pl_unhook must keep its arity");
static_assert(std::is_same_v<decltype(&GetPreloaderInput), GetInputType>,
              "GetPreloaderInput must keep its arity");
static_assert(std::is_same_v<decltype(&pl_patch_write_bytes), PatchBytesType>,
              "pl_patch_write_bytes must keep its arity");
static_assert(std::is_same_v<decltype(&pl_resolve_signature), ResolveSignatureType>,
              "pl_resolve_signature must keep its arity");

// `hook` has exactly one exported signature; a second overload would change this type.
using ExpectedHook = int (*)(pl::memory::FuncPtr, pl::memory::FuncPtr, pl::memory::FuncPtr *,
                             pl::memory::HookPriority, std::string_view);
static_assert(std::is_same_v<decltype(&pl::memory::hook), ExpectedHook>,
              "pl::memory::hook must keep its single 5-argument signature");

} // namespace

int main() {
  // The hook priority enum's numeric values are part of the ABI too: a mod passes the raw value,
  // and the legacy C enum and the C++ enum are two names for the same numbers.
  static_assert(static_cast<int>(PL_HOOK_PRIORITY_HIGHEST) == 0, "HIGHEST is 0");
  static_assert(static_cast<int>(PL_HOOK_PRIORITY_HIGH) == 100, "HIGH is 100");
  static_assert(static_cast<int>(PL_HOOK_PRIORITY_NORMAL) == 200, "NORMAL is 200");
  static_assert(static_cast<int>(PL_HOOK_PRIORITY_LOW) == 300, "LOW is 300");
  static_assert(static_cast<int>(PL_HOOK_PRIORITY_LOWEST) == 400, "LOWEST is 400");
  static_assert(static_cast<int>(pl::memory::HookPriority::Highest) == 0, "Highest matches");
  static_assert(static_cast<int>(pl::memory::HookPriority::High) == 100, "High matches");
  static_assert(static_cast<int>(pl::memory::HookPriority::Normal) == 200, "Normal matches");
  static_assert(static_cast<int>(pl::memory::HookPriority::Low) == 300, "Low matches");
  static_assert(static_cast<int>(pl::memory::HookPriority::Lowest) == 400, "Lowest matches");

  // The input interface's slots are the ABI a mod reads through; it must stay a plain aggregate.
  static_assert(std::is_standard_layout_v<PreloaderInput_Interface>,
                "the input interface is a standard-layout struct");
  static_assert(sizeof(PreloaderInput_Interface) == 6 * sizeof(void *),
                "the input interface is six function pointers");

  // Reaching this line means every assertion above compiled.
  std::printf("LegacyAbiTest: all compile-time ABI assertions passed\n");
  return 0;
}
