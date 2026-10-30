#pragma once

/**
 * @file LegacyHook.h
 * @brief The C hook entry points (`pl_hook` / `pl_unhook`).
 *
 * <p><b>Kept for binary compatibility, not for new code.</b> These are exported so a mod built
 * against the older C surface still resolves its symbols. A new mod should use
 * {@link pl::memory::hook} / {@link pl::memory::unhook} (C++, in `pl/memory/Hook.hpp`), which adds
 * a diagnostic label and returns a status; the C entry points forward to the same implementation.
 *
 * <p>Nothing in this repository calls them. Do not delete them without checking that no
 * distributed mod still links against the symbols.
 */

#include <stdbool.h>

#include "pl/legacy/LegacyMacro.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *PLFuncPtr;
typedef PLFuncPtr FuncPtr;

typedef enum PLHookPriority {
  PL_HOOK_PRIORITY_HIGHEST = 0,
  PL_HOOK_PRIORITY_HIGH = 100,
  PL_HOOK_PRIORITY_NORMAL = 200,
  PL_HOOK_PRIORITY_LOW = 300,
  PL_HOOK_PRIORITY_LOWEST = 400,
} PLHookPriority;

PL_LEGACY_EXPORT int pl_hook(PLFuncPtr target, PLFuncPtr detour,
                             PLFuncPtr *originalFunc,
                             PLHookPriority priority);

PL_LEGACY_EXPORT bool pl_unhook(PLFuncPtr target, PLFuncPtr detour);

#ifdef __cplusplus
} // extern "C"
#endif

