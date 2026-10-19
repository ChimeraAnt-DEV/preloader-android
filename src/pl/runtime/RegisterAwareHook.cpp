#include "pl/runtime/RegisterAwareHook.h"

#include <cstdint>

#if !defined(__aarch64__)
#error "RegisterAwareHook is only implemented for AArch64; the preloader builds arm64-v8a only."
#endif

using pl::runtime::PassthroughContext;

extern "C" void *RecordPassthroughCall(PassthroughContext *context) {
  if (context != nullptr) {
    context->counter.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<void *>(context->original);
  }
  return nullptr;
}

// Per-detour states, with C linkage so the assembly can reference them by their plain symbol names.
extern "C" {

PassthroughContext pl_runtime_passthrough_ctx_skinCape{};
PassthroughContext pl_runtime_passthrough_ctx_textureBind{};
PassthroughContext pl_runtime_passthrough_ctx_packetRead{};

PassthroughContext *pl_runtime_passthrough_skinCape_ptr() {
  return &pl_runtime_passthrough_ctx_skinCape;
}
PassthroughContext *pl_runtime_passthrough_textureBind_ptr() {
  return &pl_runtime_passthrough_ctx_textureBind;
}
PassthroughContext *pl_runtime_passthrough_packetRead_ptr() {
  return &pl_runtime_passthrough_ctx_packetRead;
}

// The per-detour record stubs. A veneer cannot itself take the address of its context through
// `@PAGE`/`@PAGEOFF` in inline assembly (the integrated assembler rejects the ADRP fixup for a
// data symbol), so each veneer `bl`s its own stub instead; the stub does the addressing in C++
// where the compiler emits a correct relocation, bumps the counter and returns the original
// trampoline in x0 for the veneer to tail-branch to.
void *pl_runtime_passthrough_skinCape_record() {
  return RecordPassthroughCall(&pl_runtime_passthrough_ctx_skinCape);
}
void *pl_runtime_passthrough_textureBind_record() {
  return RecordPassthroughCall(&pl_runtime_passthrough_ctx_textureBind);
}
void *pl_runtime_passthrough_packetRead_record() {
  return RecordPassthroughCall(&pl_runtime_passthrough_ctx_packetRead);
}

} // extern "C"

// ---------------------------------------------------------------------------------------------
// Return-transparent passthrough veneer (AArch64).
//
// Frame layout, pushed low-to-high (SP after each store):
//   stp q0,q1 [sp,#-32]!   q0..q7   -> 128 bytes (4 pairs of 128-bit regs, 32 bytes each)
//   stp x0,x1 ... x14,x15 [sp,#-16]! -> 128 bytes (8 pairs of 64-bit regs, 16 bytes each)
//   stp x16,x17 [sp,#-16]!           -> 16 bytes (scratch, reused for the branch target)
//   stp x18,x30 [sp,#-16]!           -> 16 bytes (platform reg + return address)
//        total     288 bytes, 16 aligned.
//
// Sequence:
//   1. save q0..q7, x0..x15, x16..x17, x18/x30
//   2. x0 = &context; bl RecordPassthroughCall (returns original trampoline in x0)
//   3. stash the trampoline into the x16 slot [sp,#16] (x16 is scratch; the original
//      function does not read it as an argument)
//   4. restore x18/x30, pick up the trampoline into x16, skip the x16/x17 slot, restore
//      x14..x0 and q0..q7
//   5. br x16  -> the original returns straight to the detour's caller; LR untouched, so
//      whatever the game function returns (x0 pointer, x8 sret, q0 FP value) survives.
//
// x19..x29 are callee-saved; RecordPassthroughCall preserves them under AAPCS, and the veneer
// never touches them, so they ride through untouched as well.

#define PASSTHROUGH_VENEER(symbol, recordStub)                                        \
  extern "C" __attribute__((naked, used, noinline)) void symbol(void) {                \
    __asm__ __volatile__(                                                              \
        /* 1. save volatile registers */                                               \
        "stp q0, q1, [sp, #-32]!\n"                                                    \
        "stp q2, q3, [sp, #-32]!\n"                                                    \
        "stp q4, q5, [sp, #-32]!\n"                                                    \
        "stp q6, q7, [sp, #-32]!\n"                                                    \
        "stp x0, x1, [sp, #-16]!\n"                                                    \
        "stp x2, x3, [sp, #-16]!\n"                                                    \
        "stp x4, x5, [sp, #-16]!\n"                                                    \
        "stp x6, x7, [sp, #-16]!\n"                                                    \
        "stp x8, x9, [sp, #-16]!\n"                                                    \
        "stp x10, x11, [sp, #-16]!\n"                                                  \
        "stp x12, x13, [sp, #-16]!\n"                                                  \
        "stp x14, x15, [sp, #-16]!\n"                                                  \
        "stp x16, x17, [sp, #-16]!\n"                                                  \
        "stp x18, x30, [sp, #-16]!\n"                                                  \
        /* 2. record + get the original trampoline: the stub does the context addressing in \
            C++ (a data-symbol ADRP fixup is rejected in inline asm) and returns the original \
            trampoline in x0. */                                                       \
        "bl " #recordStub "\n"                                                         \
        /* 3. stash the trampoline into the x16 slot (sp + 16: x16,x17 were saved there) */ \
        "str x0, [sp, #16]\n"                                                          \
        /* 4. restore, ending with sp back at entry */                                 \
        "ldp x18, x30, [sp], #16\n"                                                    \
        "ldr x16, [sp]\n" /* x16 = original trampoline */                              \
        "add sp, sp, #16\n" /* skip the x16/x17 slot */                                \
        "ldp x14, x15, [sp], #16\n"                                                    \
        "ldp x12, x13, [sp], #16\n"                                                    \
        "ldp x10, x11, [sp], #16\n"                                                    \
        "ldp x8, x9, [sp], #16\n"                                                      \
        "ldp x6, x7, [sp], #16\n"                                                      \
        "ldp x4, x5, [sp], #16\n"                                                      \
        "ldp x2, x3, [sp], #16\n"                                                      \
        "ldp x0, x1, [sp], #16\n"                                                      \
        "ldp q6, q7, [sp], #32\n"                                                      \
        "ldp q4, q5, [sp], #32\n"                                                      \
        "ldp q2, q3, [sp], #32\n"                                                      \
        "ldp q0, q1, [sp], #32\n"                                                      \
        /* 5. tail-branch to the original */                                           \
        "br x16\n"                                                                     \
        : /* no outputs */                                                             \
        : /* no inputs */                                                              \
        : "memory");                                                                   \
  }

PASSTHROUGH_VENEER(pl_runtime_passthrough_skinCape_addr,
                   pl_runtime_passthrough_skinCape_record)
PASSTHROUGH_VENEER(pl_runtime_passthrough_textureBind_addr,
                   pl_runtime_passthrough_textureBind_record)
PASSTHROUGH_VENEER(pl_runtime_passthrough_packetRead_addr,
                   pl_runtime_passthrough_packetRead_record)