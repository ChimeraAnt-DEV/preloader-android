#pragma once

/**
 * @file Hook.hpp
 * @brief Memory hook API.
 */

#include <string_view>
#include <utility>

#include "pl/Export.hpp"

namespace pl::memory {

using FuncPtr = void *;

/**
 * @brief Hook chain priority. Lower values run earlier.
 */
enum class HookPriority : int {
  Highest = 0,
  High = 100,
  Normal = 200,
  Low = 300,
  Lowest = 400,
};

/**
 * @brief Installs a detour for a target function.
 *
 * @param name Diagnostic label used in log lines (e.g. "PauseMenuOpen"). May be empty.
 * @return 0 on success, -1 on a failed/refused install.
 *
 * The name is a parameter rather than a default argument: a default argument is resolved at the
 * call site and does not emit a distinct symbol, so a library built against a shorter signature
 * would import a symbol this library does not export and fail to load. There is one exported
 * signature.
 */
PL_EXPORT int hook(FuncPtr target, FuncPtr detour, FuncPtr *originalFunc,
                   HookPriority priority, std::string_view name);

/**
 * @brief Removes a detour from a target function.
 */
PL_EXPORT bool unhook(FuncPtr target, FuncPtr detour);

/**
 * @brief RAII owner for an installed hook.
 */
class HookHandle {
public:
  HookHandle() = default;

  HookHandle(FuncPtr target, FuncPtr detour, FuncPtr *originalFunc,
             HookPriority priority = HookPriority::Normal, std::string_view name = {})
      : mTarget(target), mDetour(detour),
        mInstalled(hook(target, detour, originalFunc, priority, name) == 0) {}

  HookHandle(const HookHandle &) = delete;
  HookHandle &operator=(const HookHandle &) = delete;

  HookHandle(HookHandle &&other) noexcept { swap(other); }

  HookHandle &operator=(HookHandle &&other) noexcept {
    if (this != &other) {
      reset();
      swap(other);
    }
    return *this;
  }

  ~HookHandle() { reset(); }

  [[nodiscard]] bool installed() const noexcept { return mInstalled; }

  void reset() {
    if (mInstalled) {
      unhook(mTarget, mDetour);
      mInstalled = false;
    }
    mTarget = nullptr;
    mDetour = nullptr;
  }

  void swap(HookHandle &other) noexcept {
    std::swap(mTarget, other.mTarget);
    std::swap(mDetour, other.mDetour);
    std::swap(mInstalled, other.mInstalled);
  }

private:
  FuncPtr mTarget{};
  FuncPtr mDetour{};
  bool mInstalled{};
};

} // namespace pl::memory

