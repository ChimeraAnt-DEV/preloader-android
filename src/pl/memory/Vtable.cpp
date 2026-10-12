#include "pl/memory/Vtable.hpp"

#include <cstring>
#include <limits>
#include <mutex>
#include <string>

#include "pl/Gloss.h"
#include "pl/Logger.hpp"

namespace pl::memory {
namespace {

std::once_flag glossInitOnce;

void ensureGlossInitialized() {
  // GlossInit(false) is the trampoline/bookkeeping init and is all the RTTI scan and the
  // inline hook need. GlossInit(true) additionally runs LinkerInit(), which re-hooks the
  // Android linker's own do_dlopen/do_dlsym via an inline hook; on this build that faults
  // (the "Start hook linker..." -> SIGSEGV sequence) and it is not needed here.
  std::call_once(glossInitOnce, [] { GlossInit(false); });
}

std::string_view normalizeTypeInfoName(std::string_view typeInfoName) {
  constexpr std::string_view kTypeInfoNamePrefix = "_ZTS";
  if (typeInfoName.starts_with(kTypeInfoNamePrefix)) {
    return typeInfoName.substr(kTypeInfoNamePrefix.size());
  }
  return typeInfoName;
}

uintptr_t findBytes(uintptr_t base, size_t size, const void *pattern,
                    size_t patternSize) {
  if (!base || !pattern || patternSize == 0 || size < patternSize) {
    return 0;
  }

  const auto *patternBytes = static_cast<const uint8_t *>(pattern);
  const uint8_t first = patternBytes[0];
  const size_t lastStart = size - patternSize;
  size_t offset = 0;

  while (offset <= lastStart) {
    const size_t remainingStarts = lastStart - offset + 1;
    const void *match = std::memchr(
        reinterpret_cast<const void *>(base + offset), first, remainingStarts);
    if (!match) {
      return 0;
    }

    const auto candidate = reinterpret_cast<uintptr_t>(match);
    const size_t candidateOffset = candidate - base;
    if (std::memcmp(match, pattern, patternSize) == 0) {
      return candidate;
    }
    offset = candidateOffset + 1;
  }

  return 0;
}

bool canReadPointer(size_t sectionSize, size_t offset) {
  return offset <= sectionSize && sectionSize - offset >= sizeof(uintptr_t);
}

uintptr_t readPointer(uintptr_t base, size_t offset) {
  uintptr_t value = 0;
  std::memcpy(&value, reinterpret_cast<const void *>(base + offset),
              sizeof(value));
  return value;
}

uintptr_t findTypeInfo(uintptr_t dataRelRo, size_t dataRelRoSize,
                       uintptr_t typeName) {
  if (dataRelRoSize < sizeof(uintptr_t) * 2) {
    return 0;
  }

  for (size_t offset = sizeof(uintptr_t); canReadPointer(dataRelRoSize, offset);
       offset += sizeof(uintptr_t)) {
    if (readPointer(dataRelRo, offset) == typeName) {
      return dataRelRo + offset - sizeof(uintptr_t);
    }
  }
  return 0;
}

bool checkedSlotOffset(size_t addressPointOffset, size_t slot,
                       size_t sectionSize, size_t &slotOffset) {
  if (slot > std::numeric_limits<size_t>::max() / sizeof(uintptr_t)) {
    return false;
  }

  const size_t slotBytes = slot * sizeof(uintptr_t);
  if (addressPointOffset > std::numeric_limits<size_t>::max() - slotBytes) {
    return false;
  }

  slotOffset = addressPointOffset + slotBytes;
  return canReadPointer(sectionSize, slotOffset);
}

uintptr_t readVtableSlot(uintptr_t dataRelRo, size_t dataRelRoSize,
                         size_t typeInfoPointerOffset, size_t slot) {
  const size_t addressPointOffset = typeInfoPointerOffset + sizeof(uintptr_t);
  size_t slotOffset = 0;
  if (!checkedSlotOffset(addressPointOffset, slot, dataRelRoSize, slotOffset)) {
    return 0;
  }
  return readPointer(dataRelRo, slotOffset);
}

uintptr_t findPrimaryVtableSlot(uintptr_t dataRelRo, size_t dataRelRoSize,
                                uintptr_t typeInfo, size_t slot) {
  if (dataRelRoSize < sizeof(uintptr_t) * 3) {
    return 0;
  }

  for (size_t offset = sizeof(uintptr_t); canReadPointer(dataRelRoSize, offset);
       offset += sizeof(uintptr_t)) {
    if (readPointer(dataRelRo, offset) != typeInfo) {
      continue;
    }

    const uintptr_t offsetToTop =
        readPointer(dataRelRo, offset - sizeof(uintptr_t));
    if (offsetToTop != 0) {
      continue;
    }

    if (const uintptr_t slotValue =
            readVtableSlot(dataRelRo, dataRelRoSize, offset, slot)) {
      return slotValue;
    }
  }

  return 0;
}

} // namespace

uintptr_t resolveVtableFunction(std::string_view typeInfoName, size_t slot,
                                std::string_view moduleName) {
  if (typeInfoName.empty() || moduleName.empty()) {
    return 0;
  }

  const std::string_view normalizedName = normalizeTypeInfoName(typeInfoName);
  if (normalizedName.empty()) {
    return 0;
  }

  const std::string name(normalizedName);
  const std::string module(moduleName);

  ensureGlossInitialized();

  size_t rodataSize = 0;
  const uintptr_t rodata =
      GlossGetLibSection(module.c_str(), ".rodata", &rodataSize);
  if (!rodata || rodataSize == 0) {
    return 0;
  }

  const uintptr_t typeName =
      findBytes(rodata, rodataSize, name.c_str(), name.size() + 1);
  if (!typeName) {
    return 0;
  }

  size_t dataRelRoSize = 0;
  const uintptr_t dataRelRo =
      GlossGetLibSection(module.c_str(), ".data.rel.ro", &dataRelRoSize);
  if (!dataRelRo || dataRelRoSize == 0) {
    return 0;
  }

  const uintptr_t typeInfo = findTypeInfo(dataRelRo, dataRelRoSize, typeName);
  if (!typeInfo) {
    preloaderLogger.debug("resolveVtableFunction({}, slot {}): typeinfo not found in {}",
                          normalizedName, slot, module);
    return 0;
  }

  const uintptr_t resolved =
      findPrimaryVtableSlot(dataRelRo, dataRelRoSize, typeInfo, slot);
  preloaderLogger.debug("resolveVtableFunction({}, slot {}) -> {:#x} in {}",
                        normalizedName, slot, resolved, module);
  return resolved;
}

namespace {

// A function's prologue: `stp x29, x30, [sp, #-imm]!` (SM32 / pre-index writeback). The low 16
// bits are the register pair (x29,x30 = 0x7BFD) and the top byte is the opcode 0xA9 for a 64-bit
// pre-index store pair.
bool IsFunctionPrologue(uint32_t word) {
  return (word & 0x0000FFFFu) == 0x7BFDu && (word & 0xFF000000u) == 0xA9000000u;
}

// Scans a function body for an ADRP+ADD pair that forms `target`, stopping at the next function
// prologue so the scan never reads into a neighbouring slot's body (which is where false positives
// come from). arm64 materialises a symbol address as `adrp xN, page` then `add xN, xN, #off`; the
// pair is the load of the string's address needed to pass it to a diagnostic call.
bool FunctionReferencesAddress(uintptr_t funcStart, uintptr_t funcEnd, uintptr_t target) {
  if (funcStart == 0 || funcEnd <= funcStart || target == 0) return false;
  const uintptr_t page = target & ~static_cast<uintptr_t>(0xFFF);
  const uint32_t low = static_cast<uint32_t>(target & 0xFFF);
  const auto *code = reinterpret_cast<const uint32_t *>(funcStart);
  const std::size_t count = (funcEnd - funcStart) / sizeof(uint32_t);
  for (std::size_t i = 0; i + 1 < count; ++i) {
    const uint32_t w = code[i];
    // A new function begins at a prologue (past the first word, which is this one's own).
    if (i > 4 && IsFunctionPrologue(w)) return false;
    if ((w & 0x9F000000u) != 0x90000000u) continue;  // ADRP
    const uint32_t rd = w & 0x1Fu;
    const uint32_t immlo = (w >> 29) & 0x3u;
    const uint32_t immhi = (w >> 5) & 0x7FFFFu;
    int64_t imm = static_cast<int64_t>((immhi << 2) | immlo);
    if (imm & (1 << 20)) imm -= (1 << 21);
    const uintptr_t pc = funcStart + i * sizeof(uint32_t);
    if ((pc & ~static_cast<uintptr_t>(0xFFF)) + (imm << 12) != page) continue;
    // The next few instructions should contain `add rd, rd, #low`.
    for (std::size_t j = i + 1; j < i + 7 && j < count; ++j) {
      const uint32_t w2 = code[j];
      if ((w2 & 0xFFC00000u) != 0x91000000u) continue;  // ADD (immediate), 64-bit
      const uint32_t rn = (w2 >> 5) & 0x1Fu;
      const uint32_t rd2 = w2 & 0x1Fu;
      const uint32_t imm12 = (w2 >> 10) & 0xFFFu;
      if (rn == rd && rd2 == rd && imm12 == low) return true;
    }
  }
  return false;
}

// The next function/vtable slot's address, used to bound a scan of one slot's body.
uintptr_t NextSlotAddress(uintptr_t dataRelRo, size_t size, uintptr_t typeInfoPointerOffset,
                          size_t startSlot, size_t maxSlots, uintptr_t fallbackEnd) {
  for (size_t s = startSlot + 1; s <= maxSlots; ++s) {
    const size_t offset = typeInfoPointerOffset + sizeof(uintptr_t) + s * sizeof(uintptr_t);
    if (!canReadPointer(size, offset)) break;
    const uintptr_t value = readPointer(dataRelRo, offset);
    if (value != 0) return value;
  }
  return fallbackEnd;
}

} // namespace

uintptr_t resolveVtableSlotReferencingString(std::string_view typeInfoName,
                                             std::string_view needle,
                                             std::string_view moduleName,
                                             std::size_t maxSlots, std::size_t *outSlot) {
  if (typeInfoName.empty() || needle.empty() || moduleName.empty()) return 0;
  const std::string_view normalizedName = normalizeTypeInfoName(typeInfoName);
  if (normalizedName.empty()) return 0;
  const std::string name(normalizedName);
  const std::string module(moduleName);

  ensureGlossInitialized();

  size_t rodataSize = 0;
  const uintptr_t rodata = GlossGetLibSection(module.c_str(), ".rodata", &rodataSize);
  if (!rodata || rodataSize == 0) return 0;

  const uintptr_t typeName = findBytes(rodata, rodataSize, name.c_str(), name.size() + 1);
  if (!typeName) return 0;

  size_t dataRelRoSize = 0;
  const uintptr_t dataRelRo = GlossGetLibSection(module.c_str(), ".data.rel.ro", &dataRelRoSize);
  if (!dataRelRo || dataRelRoSize == 0) return 0;

  const uintptr_t typeInfo = findTypeInfo(dataRelRo, dataRelRoSize, typeName);
  if (!typeInfo) return 0;

  // The needle is a NUL-terminated diagnostic string; find its address in .rodata.
  const uintptr_t needleAddr = findBytes(rodata, rodataSize, needle.data(), needle.size() + 1);
  if (!needleAddr) {
    preloaderLogger.debug("resolveVtableSlotReferencingString: needle '{}' not in .rodata",
                          std::string(needle));
    return 0;
  }

  size_t textSize = 0;
  const uintptr_t text = GlossGetLibSection(module.c_str(), ".text", &textSize);
  const uintptr_t textEnd = text ? text + textSize : 0;

  // Walk the primary vtable. Its start is the typeinfo pointer at `typeInfo`; slot 0 is the first
  // entry after it. The scan window is generous; the real bound is the next function prologue
  // (`FunctionReferencesAddress` stops there), so a large window cannot read into a neighbouring
  // slot's body. Verified against 1.21.132, 26.52 and 26.60.28: all three resolve slot 17.
  constexpr uintptr_t kScanWindow = 0x4000;
  const size_t typeInfoPointerOffset = typeInfo - dataRelRo;
  for (size_t slot = 0; slot <= maxSlots; ++slot) {
    const size_t offset =
        typeInfoPointerOffset + sizeof(uintptr_t) + slot * sizeof(uintptr_t);
    if (!canReadPointer(dataRelRoSize, offset)) break;
    const uintptr_t fn = readPointer(dataRelRo, offset);
    if (fn == 0 || (text && (fn < text || fn >= textEnd))) continue;
    const uintptr_t next = NextSlotAddress(dataRelRo, dataRelRoSize, typeInfoPointerOffset,
                                           slot, maxSlots, textEnd);
    uintptr_t funcEnd = fn + kScanWindow;
    if (next > fn && next < funcEnd) funcEnd = next;
    if (textEnd && funcEnd > textEnd) funcEnd = textEnd;
    if (FunctionReferencesAddress(fn, funcEnd, needleAddr)) {
      if (outSlot) *outSlot = slot;
      preloaderLogger.info("Resolved {} slot {} by string '{}'", name, slot,
                           std::string(needle));
      return fn;
    }
  }
  preloaderLogger.debug("resolveVtableSlotReferencingString: no slot of {} references '{}'", name,
                        std::string(needle));
  return 0;
}

} // namespace pl::memory
