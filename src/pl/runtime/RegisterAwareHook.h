#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

/**
 * @file RegisterAwareHook.h
 * @brief Return-transparent passthrough detours for unknown ABIs (ARM64).
 *
 * The cosmetics seams (skin/cape accessor, texture binder, player-join packet handler) have
 * signatures the stripped game binary does not expose, so the detours are pure passthroughs: they
 * record that the hook fired and then forward to the original. A C++ detour that <em>calls</em> the
 * original through a function pointer does not preserve the game function's return value in general
 * — an `x0` result only survives by accident (the compiler is free to reuse the register), and an
 * `x8` indirect-result (sret) is a caller-saved register that the call clobbers outright.
 *
 * The veneers in this module are the fix: each saves every volatile register (the integer
 * argument/return set, the indirect-result register `x8`, LR, and the `q0..q7` FP/SIMD
 * argument/return set), bumps its call counter through a tiny C++ helper, restores every volatile
 * register, and then **branches** to the original trampoline. Branching rather than calling means
 * the original returns directly to the detour's caller with LR untouched, so whatever the game
 * function returns — an `x0` pointer, an `x8` sret, a `q0` double — reaches the caller exactly as
 * if the hook were not installed.
 *
 * One assembled veneer is emitted per passthrough via {@link PASSTHROUGH_DETOUR}, each referencing
 * its own {@link PassthroughContext}, so two detours never share mutable state.
 */

namespace pl::runtime {

/** The per-detour state a veneer reads: where to record the call and where the original lives. */
struct PassthroughContext {
  /** Incremented on every forwarded call. */
  std::atomic<std::uint32_t> counter{0};
  /** The original trampoline handed back by {@code pl::memory::hook}. */
  void (*original)(void) = nullptr;
  /** Human-readable label for the log line and the diagnostics string. */
  const char *label = "";
};

} // namespace pl::runtime

/**
 * @brief The C++ half of a passthrough veneer: records the call and returns the original address.
 *
 * Only ever called from the assembly veneers (never directly). Returns the original trampoline so
 * the veneer can branch to it after restoring the volatile registers. Has C linkage so a veneer's
 * `bl`s land on the plain symbol.
 */
extern "C" void *RecordPassthroughCall(pl::runtime::PassthroughContext *context);

extern "C" pl::runtime::PassthroughContext pl_runtime_passthrough_ctx_skinCape;
extern "C" pl::runtime::PassthroughContext pl_runtime_passthrough_ctx_textureBind;
extern "C" pl::runtime::PassthroughContext pl_runtime_passthrough_ctx_packetRead;

extern "C" pl::runtime::PassthroughContext *pl_runtime_passthrough_skinCape_ptr();
extern "C" pl::runtime::PassthroughContext *pl_runtime_passthrough_textureBind_ptr();
extern "C" pl::runtime::PassthroughContext *pl_runtime_passthrough_packetRead_ptr();

// The register-preserving veneer entry points (naked aarch64 functions). Detour targets passed to
// {@code pl::memory::hook} in place of the C++ passthroughs they replace.
extern "C" void pl_runtime_passthrough_skinCape_addr(void);
extern "C" void pl_runtime_passthrough_textureBind_addr(void);
extern "C" void pl_runtime_passthrough_packetRead_addr(void);

// The per-detour record stubs the veneers branch to. Each records the call against its own
// PassthroughContext and returns the original trampoline (x0) for the veneer to tail-branch to.
// Declared here so any TU can reference them (only RegisterAwareHook.cpp defines them).
extern "C" void *pl_runtime_passthrough_skinCape_record(void);
extern "C" void *pl_runtime_passthrough_textureBind_record(void);
extern "C" void *pl_runtime_passthrough_packetRead_record(void);