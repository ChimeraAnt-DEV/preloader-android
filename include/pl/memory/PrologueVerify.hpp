#pragma once

/**
 * @file PrologueVerify.hpp
 * @brief The pure AArch64 rule that decides whether a hook install actually patched its target.
 *
 * <p>A hook backend can report success and still write nothing (or write to the wrong address). The
 * only way to catch that is to read the first instruction back after the install and check it looks
 * like a redirection. The decision is pure (an opcode in, a verdict out) and lives here so it is
 * host-tested rather than only exercised on a device, where a wrong verdict means a detour that is
 * silently not live.
 *
 * <p>The exact encoding the backend emits varies - an absolute jump is {@code LDR X18, #8; BR X18;
 * <dest>}, an in-range jump is a plain {@code B} - so the accepted set is deliberately broad, and
 * the one instruction that definitely means "nothing was patched" is a {@code NOP}.
 */

#include <cstdint>

namespace pl::memory {

/** What the first instruction at a hooked target says about the install. */
enum class PrologueVerdict {
  /** A branch-family instruction: the patch took. */
  Patched,
  /** A NOP: the backend replaced the entry with a no-op, so nothing was patched. */
  NotPatched,
  /** Not a branch-family opcode and not a NOP: unrecognised, treat as not verified. */
  Unrecognised,
};

/** The `NOP` the backend writes when it patches nothing. */
constexpr std::uint32_t kA64Nop = 0xD503201FU;

/**
 * @brief Whether an AArch64 instruction looks like a redirection.
 *
 * <p>Accepting {@code RET} is deliberately excluded: an unpatched leaf function legitimately starts
 * with one, so counting it as a branch would report a failed install as patched.
 */
inline bool IsA64BranchLike(std::uint32_t insn) {
  // B / BL (immediate). The opcode differs only in bit 31 (B = 000101, BL = 100101), so the mask
  // clears bit 31 and matches both. Masking with 0xFC000000 matched B only, which is why a real BL
  // install (a branch-with-link veneer) read as unverified.
  if ((insn & 0x7C000000U) == 0x14000000U) return true;
  // BR / BLR (register). The family is 1101011 with opc (bits 24:21) 0 = BR, 1 = BLR, 2 = RET.
  // RET is excluded because an unpatched leaf function legitimately starts with one. The old
  // 0xFFFFFC1F mask matched BR only, so a BLR install also read as unverified.
  if ((insn & 0xFE000000U) == 0xD6000000U) {
    const std::uint32_t opc = (insn >> 21) & 0xFU;
    if (opc == 0U || opc == 1U) return true;
  }
  // B.cond.
  if ((insn & 0xFF000010U) == 0x54000000U) return true;
  // CBZ / CBNZ.
  if ((insn & 0x7F000000U) == 0x34000000U) return true;
  // TBZ / TBNZ.
  if ((insn & 0x7F000000U) == 0x36000000U) return true;
  // LDR (literal) - the `LDR X18, #8` an absolute jump emits.
  if ((insn & 0x3B000000U) == 0x18000000U) return true;
  // ADRP - the absolute-jump prologue.
  if ((insn & 0x9F000000U) == 0x90000000U) return true;
  return false;
}

/** @brief Classifies the first instruction so the caller can log the exact reason. */
inline PrologueVerdict ClassifyPrologue(std::uint32_t first) {
  if (first == kA64Nop) return PrologueVerdict::NotPatched;
  return IsA64BranchLike(first) ? PrologueVerdict::Patched : PrologueVerdict::Unrecognised;
}

} // namespace pl::memory
