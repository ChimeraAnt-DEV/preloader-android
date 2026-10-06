#include "pl/memory/HookTarget.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>

namespace pl::memory {
namespace {

bool parseMapsLine(const char *line, uintptr_t &start, uintptr_t &end,
                   char perms[5], std::string &path) {
  char permsBuf[5] = {};
  if (std::sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &start, &end,
                  permsBuf) != 3) {
    return false;
  }
  if (end <= start) {
    return false;
  }

  std::memcpy(perms, permsBuf, sizeof(permsBuf));

  // Everything after the "dev inode" columns is the mapping path (may be empty
  // for anonymous mappings).
  path.clear();
  const char *cursor = line;
  int fields = 0;
  while (*cursor != '\0' && fields < 5) {
    while (*cursor == ' ') {
      ++cursor;
    }
    while (*cursor != '\0' && *cursor != ' ') {
      ++cursor;
    }
    ++fields;
  }
  while (*cursor == ' ') {
    ++cursor;
  }
  const char *eol = cursor;
  while (*eol != '\0' && *eol != '\n') {
    ++eol;
  }
  path.assign(cursor, static_cast<std::size_t>(eol - cursor));
  return true;
}

} // namespace

std::string MemoryRegionInfo::describe() const {
  if (!mapped) {
    return "unmapped";
  }

  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), "%s %#" PRIxPTR "-%#" PRIxPTR " %s",
                perms, start, end, path.empty() ? "<anon>" : path.c_str());
  std::string result(buffer);
  if (!symbolName.empty()) {
    result.append(" (+").append(symbolName).append(")");
  }
  return result;
}

MemoryRegionInfo queryMemoryRegion(uintptr_t address) {
  MemoryRegionInfo info;
  if (address == 0) {
    return info;
  }

  FILE *maps = std::fopen("/proc/self/maps", "r");
  if (maps == nullptr) {
    return info;
  }

  char line[4096];
  while (std::fgets(line, sizeof(line), maps)) {
    uintptr_t start = 0;
    uintptr_t end = 0;
    char perms[5] = {};
    std::string path;
    if (!parseMapsLine(line, start, end, perms, path)) {
      continue;
    }
    if (address >= start && address < end) {
      info.mapped = true;
      info.start = start;
      info.end = end;
      std::memcpy(info.perms, perms, sizeof(info.perms));
      info.path = std::move(path);
      break;
    }
  }
  std::fclose(maps);

  if (info.mapped) {
    Dl_info dlInfo{};
    if (dladdr(reinterpret_cast<void *>(address), &dlInfo) != 0 &&
        dlInfo.dli_sname != nullptr) {
      info.symbolName = dlInfo.dli_sname;
    }
  }
  return info;
}

bool isHookAddressSane(uintptr_t address) {
  if (address == 0) {
    return false;
  }
#if defined(__arm__)
  // A Thumb function pointer carries its instruction-set mode in bit 0, so validate the
  // code address with that bit cleared (and require 2-byte alignment).
  const uintptr_t codeAddress = address & ~static_cast<uintptr_t>(1);
  return (codeAddress % 2) == 0;
#else
  return (address % kInstructionAlignment) == 0;
#endif
}

} // namespace pl::memory
