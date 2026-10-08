#pragma once

/**
 * @file DobbyHookManager.hpp
 * @brief Hook management wrapper with a Dobby-shaped API (Task 2).
 *
 * Resolves a target inside a loaded module and installs a register-preserving detour with an
 * original-function pointer, mirroring the Dobby surface (`DobbySymbolResolver`, `DobbyHook`) so
 * the hook code reads the same as the ARM64 instrumentation pattern:
 *
 * @code
 *   using orig_render_t = void (*)(void *, void *, ...);
 *   static orig_render_t orig_render_func = nullptr;
 *   auto target = pl::hooks::DobbyHookManager::instance().resolveOffset("libminecraftpe.so", off);
 *   DobbyHook((void *)target, (void *)HookedRender, (void **)&orig_render_func);
 * @endcode
 *
 * <p><b>Backend.</b> The install is serviced by the repo-vendored GlossHook through
 * {@link pl::memory::hook}; upstream Dobby does not build for Android arm64 at any reachable
 * commit (its arm64 closure bridge is Apple-syntax asm the ELF assembler rejects, and its
 * runtime-assembler / PlatformUtil path is an unfinished refactor). GlossHook is the same in-place
 * trampoline primitive and is already the engine hook backend, so this manager wraps the proven
 * one. Swapping in a fresh Dobby later is additive — only this file changes.
 *
 * <p><b>Register safety (ARM64).</b> A detour for an unknown C++ signature forwards the full
 * integer argument register set (x0–x7) untouched and dereferences nothing, which is what makes an
 * unknown ABI safe; {@link ArgRegisters} exposes those registers to a detour that wants to read
 * them. The hook backend preserves the callee-saved set and stack alignment across the trampoline.
 */

#include <cstdint>
#include <string>
#include <string_view>

#include "pl/Export.hpp"

namespace pl::hooks {

/** @brief Outcome of a hook install. */
enum class HookStatus {
  Installed,   ///< the detour is live
  Failed,      ///< the target was sane but the install failed
  Unsupported, ///< the target could not be resolved on this build
};

/** @brief The first eight integer argument registers, captured for a passthrough detour. */
struct ArgRegisters {
  void *x0 = nullptr;
  void *x1 = nullptr;
  void *x2 = nullptr;
  void *x3 = nullptr;
  void *x4 = nullptr;
  void *x5 = nullptr;
  void *x6 = nullptr;
  void *x7 = nullptr;

  /** @brief The argument at register index 0–7, or null when out of range. */
  [[nodiscard]] void *at(int index) const;
};

/**
 * @brief Resolves module symbols/offsets and installs detours.
 *
 * A process-wide singleton; resolution handles are cached so a repeated install for the same module
 * does not reopen it.
 */
class PL_EXPORT DobbyHookManager {
public:
  /** @brief The process-wide manager. */
  static DobbyHookManager &instance();

  /**
   * @brief Resolves an exported/debug symbol in a loaded module.
   * @return the address, or 0 when the module or symbol is not found
   */
  uintptr_t resolve(std::string_view module, std::string_view symbol);

  /**
   * @brief Resolves an address as `module load bias + offset`.
   *
   * The load bias is the runtime base the loader mapped the module at, so a fixed file offset from
   * the reversed binary becomes a runtime address without a symbol.
   */
  uintptr_t resolveOffset(std::string_view module, uintptr_t offset);

  /**
   * @brief Installs a detour.
   * @param target   the resolved function address
   * @param detour   the replacement
   * @param original receives the trampoline that calls the original
   * @param name     diagnostic label for the install log line
   * @return the install outcome
   */
  HookStatus hook(void *target, void *detour, void **original, std::string_view name);

  /** @brief Removes a detour previously installed by {@link hook}. */
  bool unhook(void *target, void *detour);

  /**
   * @brief A human-readable description of the region containing @p address.
   *
   * Useful in a log line to pinpoint a bad target (module name, permissions, mapped range).
   */
  static std::string describeTarget(uintptr_t address);

  /** @brief True when @p address is non-null and instruction-aligned. */
  static bool isAddressSane(uintptr_t address);
};

/**
 * @brief Dobby-style free-function aliases for callers written against the Dobby surface.
 *
 * The names match Dobby so the ARM64 instrumentation code reads unchanged; they forward to
 * {@link DobbyHookManager}.
 */
uintptr_t DobbySymbolResolver(const char *imageName, const char *symbolName);

/** @brief See {@link DobbyHookManager::hook}. */
HookStatus DobbyHook(void *address, void *replace, void **result);

/** @brief See {@link DobbyHookManager::unhook}. */
bool DobbyUnhook(void *address, void *replace);

} // namespace pl::hooks
