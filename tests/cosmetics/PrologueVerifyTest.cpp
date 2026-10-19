// Host test for the hook-install prologue rule (item 7). Pure C++, no device.
#include <cstdio>

#include "pl/memory/PrologueVerify.hpp"

using pl::memory::ClassifyPrologue;
using pl::memory::IsA64BranchLike;
using pl::memory::kA64Nop;
using pl::memory::PrologueVerdict;

static int checks = 0;
static int failures = 0;

static void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

int main() {
  // A NOP means nothing was patched, whatever else it looks like.
  expect(ClassifyPrologue(kA64Nop) == PrologueVerdict::NotPatched, "NOP is not-patched");

  // The instructions a real install emits: B/BL, BR/BLR, LDR-literal, ADRP.
  expect(ClassifyPrologue(0x14000000U) == PrologueVerdict::Patched, "B is patched");
  expect(ClassifyPrologue(0x94000000U) == PrologueVerdict::Patched, "BL is patched");
  expect(ClassifyPrologue(0xD61F0000U) == PrologueVerdict::Patched, "BR is patched");
  expect(ClassifyPrologue(0xD63F0000U) == PrologueVerdict::Patched, "BLR is patched");
  expect(ClassifyPrologue(0x58000000U) == PrologueVerdict::Patched, "LDR literal is patched");
  expect(ClassifyPrologue(0x90000000U) == PrologueVerdict::Patched, "ADRP is patched");
  // Condition/branch-compare forms.
  expect(ClassifyPrologue(0x54000000U) == PrologueVerdict::Patched, "B.cond is patched");
  expect(ClassifyPrologue(0x34000000U) == PrologueVerdict::Patched, "CBZ is patched");
  expect(ClassifyPrologue(0x36000000U) == PrologueVerdict::Patched, "TBZ is patched");

  // RET must NOT read as patched: an unpatched leaf function starts with one, and counting it would
  // report a failed install as live.
  expect(!IsA64BranchLike(0xD65F03C0U), "RET is not branch-like");
  expect(ClassifyPrologue(0xD65F03C0U) == PrologueVerdict::Unrecognised, "RET is unrecognised");

  // A plausible unpatched prologue (stp x29, x30, [sp, #-16]!) is not a branch.
  expect(ClassifyPrologue(0xA9BF7BFDU) == PrologueVerdict::Unrecognised,
         "a stp prologue is unrecognised");

  // 0 is not a NOP, and not a branch.
  expect(ClassifyPrologue(0U) == PrologueVerdict::Unrecognised, "zero is unrecognised");

  std::printf("PrologueVerifyTest: %d checks passed\n", checks);
  return failures == 0 ? 0 : 1;
}
