#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <mimalloc.h>

#include "pl/Logger.hpp"
#include "pl/runtime/OptifineMode.h"

/**
 * @file OptifineAllocator.cpp
 * @brief Tier-1 allocator replacement: route the game's allocator calls to mimalloc.
 *
 * The interposition technique (the same one LeviLamina uses): the preloader is loaded before
 * the game, so the dynamic linker resolves the game's `malloc`/`free`/... calls to these
 * definitions, and every call is forwarded to a statically linked mimalloc.
 *
 * Correctness rules that make this safe:
 *
 *  - **The active flag is turned on by a constructor, before `JNI_OnLoad`.** `JNI_OnLoad` logs,
 *    and the logger formats a string, which allocates. If the switch were flipped later, that
 *    very log would be the first mimalloc allocation. A `__attribute__((constructor))` runs at
 *    library load, before any JNI call, so the probe and the switch happen first.
 *  - **The re-entrancy guard is on the thread, not global.** `mi_malloc` may lazily create its
 *    own heap on first use, which can call back into `malloc`; the `thread_local` guard routes
 *    that inner call to the system allocator, so mimalloc's own setup never recurses into an
 *    uninitialised mimalloc.
 *  - **Only definitions, no `dlsym(RTLD_NEXT)` on the hot path.** The system allocators are
 *    resolved lazily only as a fallback while mimalloc is not active.
 *  - **The whole C11/glibc allocation family is covered**, including the memalign variants and
 *    `malloc_usable_size`; covering only `malloc`/`free` leaves blocks allocated by one
 *    allocator and freed by the other, which corrupts.
 *  - **A probe gates the switch.** A round-trip allocate/write/read/free confirms mimalloc is
 *    usable before it is trusted with the process; if it fails, the system allocator is kept.
 *
 * Fail-safe: while mimalloc is not active the functions forward to the system allocator, so a
 * broken mimalloc can never make the process unallocatable.
 */

namespace {

/** Set true only after mimalloc has been verified usable (by the load-time constructor). */
volatile bool gAllocatorActive = false;
/**
 * Re-entrancy guard: true while inside a mimalloc call on this thread. If mimalloc re-enters
 * `malloc` during its own initialisation, the inner call falls through to the system allocator.
 */
thread_local bool tInMimalloc = false;

using MallocFn = void *(*)(std::size_t);
using CallocFn = void *(*)(std::size_t, std::size_t);
using ReallocFn = void *(*)(void *, std::size_t);
using FreeFn = void (*)(void *);
using AlignedFn = void *(*)(std::size_t, std::size_t);
using PosixMemalignFn = int (*)(void **, std::size_t, std::size_t);
using UsableSizeFn = std::size_t (*)(const void *);

MallocFn gSystemMalloc = nullptr;
CallocFn gSystemCalloc = nullptr;
ReallocFn gSystemRealloc = nullptr;
FreeFn gSystemFree = nullptr;
AlignedFn gSystemAlignedAlloc = nullptr;
PosixMemalignFn gSystemPosixMemalign = nullptr;
UsableSizeFn gSystemUsableSize = nullptr;

void EnsureSystemAllocators() {
  if (gSystemMalloc != nullptr) return;
  gSystemMalloc = reinterpret_cast<MallocFn>(dlsym(RTLD_NEXT, "malloc"));
  gSystemCalloc = reinterpret_cast<CallocFn>(dlsym(RTLD_NEXT, "calloc"));
  gSystemRealloc = reinterpret_cast<ReallocFn>(dlsym(RTLD_NEXT, "realloc"));
  gSystemFree = reinterpret_cast<FreeFn>(dlsym(RTLD_NEXT, "free"));
  gSystemAlignedAlloc = reinterpret_cast<AlignedFn>(dlsym(RTLD_NEXT, "aligned_alloc"));
  gSystemPosixMemalign =
      reinterpret_cast<PosixMemalignFn>(dlsym(RTLD_NEXT, "posix_memalign"));
  gSystemUsableSize =
      reinterpret_cast<UsableSizeFn>(dlsym(RTLD_NEXT, "malloc_usable_size"));
}

/**
 * @brief Probes mimalloc before trusting it with the whole process.
 *
 * Catches a mimalloc that failed to initialise, which would otherwise hand out pointers that
 * crash on first use with no diagnosable cause. The probe runs with the re-entrancy guard set
 * so mimalloc's own lazy setup cannot recurse.
 */
bool ProbeMimalloc() {
  if (tInMimalloc) return false;
  tInMimalloc = true;
  void *block = mi_malloc(64);
  bool ok = false;
  if (block != nullptr) {
    std::memset(block, 0xAB, 64);
    ok = static_cast<unsigned char *>(block)[0] == 0xAB &&
         static_cast<unsigned char *>(block)[63] == 0xAB;
    mi_free(block);
  }
  tInMimalloc = false;
  return ok;
}

/** Flips the allocator on at library load, before any logging or JNI call can allocate. */
__attribute__((constructor)) void InitAllocatorAtLoad() {
  gAllocatorActive = ProbeMimalloc();
}

} // namespace

extern "C" {

void *malloc(std::size_t size) {
  if (gAllocatorActive && !tInMimalloc) {
    tInMimalloc = true;
    void *ptr = mi_malloc(size);
    tInMimalloc = false;
    return ptr;
  }
  EnsureSystemAllocators();
  return gSystemMalloc ? gSystemMalloc(size) : nullptr;
}

void *calloc(std::size_t count, std::size_t size) {
  if (gAllocatorActive && !tInMimalloc) {
    tInMimalloc = true;
    void *ptr = mi_calloc(count, size);
    tInMimalloc = false;
    return ptr;
  }
  EnsureSystemAllocators();
  return gSystemCalloc ? gSystemCalloc(count, size) : nullptr;
}

void *realloc(void *ptr, std::size_t size) {
  if (gAllocatorActive && !tInMimalloc) {
    tInMimalloc = true;
    void *result = mi_realloc(ptr, size);
    tInMimalloc = false;
    return result;
  }
  EnsureSystemAllocators();
  return gSystemRealloc ? gSystemRealloc(ptr, size) : nullptr;
}

void free(void *ptr) {
  if (gAllocatorActive && !tInMimalloc) {
    tInMimalloc = true;
    mi_free(ptr);
    tInMimalloc = false;
    return;
  }
  EnsureSystemAllocators();
  if (gSystemFree) gSystemFree(ptr);
}

void *aligned_alloc(std::size_t alignment, std::size_t size) {
  if (gAllocatorActive && !tInMimalloc) {
    tInMimalloc = true;
    void *ptr = mi_aligned_alloc(alignment, size);
    tInMimalloc = false;
    return ptr;
  }
  EnsureSystemAllocators();
  return gSystemAlignedAlloc ? gSystemAlignedAlloc(alignment, size) : nullptr;
}

int posix_memalign(void **out, std::size_t alignment, std::size_t size) {
  if (gAllocatorActive && !tInMimalloc) {
    tInMimalloc = true;
    const int result = mi_posix_memalign(out, alignment, size);
    tInMimalloc = false;
    return result;
  }
  EnsureSystemAllocators();
  return gSystemPosixMemalign ? gSystemPosixMemalign(out, alignment, size) : ENOMEM;
}

/** glibc's legacy aligned allocators, which some libraries still call. */
void *memalign(std::size_t alignment, std::size_t size) {
  return aligned_alloc(alignment, size);
}

void *valloc(std::size_t size) { return aligned_alloc(4096, size); }

void *pvalloc(std::size_t size) {
  const std::size_t page = 4096;
  const std::size_t rounded = ((size + page - 1) / page) * page;
  return aligned_alloc(page, rounded == 0 ? page : rounded);
}

// The declaration in malloc.h is `size_t malloc_usable_size(const void*)`, so the parameter
// must be const-qualified to be a valid interposing definition rather than a conflicting type.
std::size_t malloc_usable_size(const void *ptr) {
  if (gAllocatorActive && !tInMimalloc) {
    tInMimalloc = true;
    const std::size_t size = mi_usable_size(ptr);
    tInMimalloc = false;
    return size;
  }
  EnsureSystemAllocators();
  return gSystemUsableSize ? gSystemUsableSize(ptr) : 0;
}

} // extern "C"

namespace pl::runtime {
namespace {

/** Human-readable mimalloc version, e.g. "2.1.7" (the header only exposes the numeric macro). */
std::string MimallocVersionString() {
  const int version = mi_version();
  return std::to_string(version / 100) + "." + std::to_string((version / 10) % 10) + "." +
         std::to_string(version % 10);
}

void ApplyAllocator(bool enabled, OptifineItemState &state) {
  if (!enabled) return;
  if (gAllocatorActive) {
    state.status = 1;
    state.detail = "mimalloc " + MimallocVersionString() + " (interposed)";
    return;
  }
  // The load-time probe already failed, or the allocator has not initialised yet. Re-probe
  // here so a transient failure at load can still recover.
  if (ProbeMimalloc()) {
    gAllocatorActive = true;
    state.status = 1;
    state.detail = "mimalloc " + MimallocVersionString() + " (interposed)";
    preloaderLogger.info("Optifine allocator: interposition active (mimalloc {})",
                         MimallocVersionString());
    return;
  }
  state.status = 3;
  state.detail = "mimalloc probe failed; system allocator kept";
  preloaderLogger.error("Optifine allocator: mimalloc probe failed");
}

struct Registrar {
  Registrar() { RegisterOptifineItem(OptifineItem::Allocator, false, &ApplyAllocator); }
};
Registrar gRegistrar;

} // namespace
} // namespace pl::runtime
