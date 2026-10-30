#include "pl/memory/Signature.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <dlfcn.h>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <sys/uio.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/memory/ElfBuildId.hpp"
#include "pl/memory/ModulePathMatch.hpp"

namespace pl::memory {
namespace {

constexpr size_t kMaxExactAnchorSize = 8;

struct PatternByte {
  uint8_t value = 0;
  uint8_t mask = 0;
};

struct ParsedPattern {
  std::vector<PatternByte> bytes;
  std::vector<size_t> checkIndices;
  size_t anchorIndex = 0;
  size_t anchorSize = 1;
};

struct MemoryRegion {
  uintptr_t start = 0;
  uintptr_t end = 0;
  bool executable = false;
};

struct ModuleInfo {
  /** Readable regions, in map order. */
  std::vector<MemoryRegion> regions;
  /** The executable subset of `regions`; code signatures are scanned only here. */
  std::vector<MemoryRegion> execRegions;
  /** The lowest mapped address, used as the identity of this load. */
  uintptr_t base = 0;
  /** The ELF build id, or empty when it could not be read. Changes on every relink. */
  std::string buildId;
  void *handle = nullptr;
};

struct CompiledPattern {
  std::string signature;
  ParsedPattern pattern;
};

struct AnchorNode {
  std::array<int, 256> next{};
  int failure = 0;
  std::vector<size_t> outputs;

  AnchorNode() { next.fill(-1); }
};

std::unordered_map<std::string, uintptr_t> sigCache;
std::unordered_map<std::string, ParsedPattern> patternCache;
std::shared_mutex cacheMutex;

int hexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool parsePatternToken(std::string_view token, PatternByte &byte) {
  if (token == "?" || token == "??") {
    byte = PatternByte{0, 0};
    return true;
  }
  if (token.size() != 2) return false;

  uint8_t value = 0;
  uint8_t mask = 0;
  for (size_t i = 0; i < token.size(); ++i) {
    const char ch = token[i];
    const auto shift = static_cast<uint8_t>((1 - i) * 4);
    if (ch == '?') continue;
    const int digit = hexValue(ch);
    if (digit < 0) return false;
    value |= static_cast<uint8_t>(digit << shift);
    mask |= static_cast<uint8_t>(0xF << shift);
  }

  byte = PatternByte{value, mask};
  return true;
}

void appendPatternByte(ParsedPattern &pattern, PatternByte byte) {
  const size_t index = pattern.bytes.size();
  if (byte.mask != 0) pattern.checkIndices.push_back(index);
  pattern.bytes.push_back(byte);
}

bool appendPatternToken(std::string_view token, ParsedPattern &pattern) {
  PatternByte byte{};
  if (parsePatternToken(token, byte)) {
    appendPatternByte(pattern, byte);
    return true;
  }
  if (token.empty() || token.size() % 2 != 0) return false;

  for (size_t pos = 0; pos < token.size(); pos += 2) {
    if (!parsePatternToken(token.substr(pos, 2), byte)) return false;
    appendPatternByte(pattern, byte);
  }
  return true;
}

ParsedPattern parsePattern(std::string_view signature) {
  ParsedPattern pattern;
  size_t pos = 0;
  while (pos < signature.size()) {
    while (pos < signature.size() &&
           std::isspace(static_cast<unsigned char>(signature[pos]))) {
      ++pos;
    }
    if (pos >= signature.size()) break;

    const size_t start = pos;
    while (pos < signature.size() &&
           !std::isspace(static_cast<unsigned char>(signature[pos]))) {
      ++pos;
    }
    if (!appendPatternToken(signature.substr(start, pos - start), pattern)) {
      pattern.bytes.clear();
      pattern.checkIndices.clear();
      break;
    }
  }
  return pattern;
}

bool matches(PatternByte pattern, uint8_t value) {
  return (value & pattern.mask) == pattern.value;
}

bool isExactByte(PatternByte byte) { return byte.mask == 0xFF; }

int maskBits(uint8_t mask) {
  int count = 0;
  while (mask != 0) {
    mask &= static_cast<uint8_t>(mask - 1);
    ++count;
  }
  return count;
}

void selectAnchor(ParsedPattern &pattern) {
  if (pattern.checkIndices.empty()) return;

  size_t bestStart = 0;
  size_t bestSize = 0;
  for (size_t runStart = 0; runStart < pattern.bytes.size();) {
    if (!isExactByte(pattern.bytes[runStart])) {
      ++runStart;
      continue;
    }

    size_t runEnd = runStart + 1;
    while (runEnd < pattern.bytes.size() &&
           isExactByte(pattern.bytes[runEnd])) {
      ++runEnd;
    }

    const size_t size = std::min(runEnd - runStart, kMaxExactAnchorSize);
    const size_t start = runEnd - size;
    if (size > bestSize || (size == bestSize && start > bestStart)) {
      bestStart = start;
      bestSize = size;
    }
    runStart = runEnd;
  }

  if (bestSize != 0) {
    pattern.anchorIndex = bestStart;
    pattern.anchorSize = bestSize;
    return;
  }

  size_t bestIndex = pattern.checkIndices.front();
  int bestBits = maskBits(pattern.bytes[bestIndex].mask);
  for (const size_t index : pattern.checkIndices) {
    const int bits = maskBits(pattern.bytes[index].mask);
    if (bits > bestBits || (bits == bestBits && index > bestIndex)) {
      bestIndex = index;
      bestBits = bits;
    }
  }
  pattern.anchorIndex = bestIndex;
  pattern.anchorSize = 1;
}

// Defined below; forward-declared so module collection can probe the ELF header.
size_t ReadRegionSafely(uintptr_t start, size_t size, uint8_t *out);

// Whether a module's regions are actually mapped at the addresses we would scan.
bool RegionsAreMapped(const ModuleInfo &module) {
  if (module.regions.empty()) return false;
  unsigned char probe = 0;
  for (const auto &region : module.regions) {
    iovec local{&probe, 1};
    iovec remote{reinterpret_cast<void *>(region.start), 1};
    if (::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0) != 1) {
      return false;
    }
  }
  return true;
}

// Collects a module's readable regions, and its executable subset, by matching the **basename** of
// the maps path column exactly. A `strstr` on the whole line also matches a path that merely
// contains the name (`libpreloader_extra.so` for `libpreloader.so`), which would scan the wrong
// regions and can resolve a pattern into an unrelated library.
bool getModuleInfo(const std::string &name, ModuleInfo &out) {
  FILE *maps = std::fopen("/proc/self/maps", "r");
  if (!maps) return false;

  bool first = true;
  char line[4096];
  while (std::fgets(line, sizeof(line), maps)) {
    MapsLine parsed{};
    if (!ParseMapsLine(line, parsed)) continue;
    if (!MapsPathNamesModule(parsed.path, name)) continue;
    if (!MapsPermsReadable(parsed.perms)) continue;

    const bool executable = MapsPermsExecutable(parsed.perms);
    if (!out.regions.empty() && out.regions.back().end == parsed.start &&
        out.regions.back().executable == executable) {
      out.regions.back().end = parsed.end;
    } else {
      out.regions.push_back(MemoryRegion{parsed.start, parsed.end, executable});
    }
    if (executable) {
      if (!out.execRegions.empty() && out.execRegions.back().end == parsed.start) {
        out.execRegions.back().end = parsed.end;
      } else {
        out.execRegions.push_back(MemoryRegion{parsed.start, parsed.end, true});
      }
    }
    if (first || parsed.start < out.base) out.base = parsed.start;
    first = false;
  }
  std::fclose(maps);
  if (out.regions.empty()) return false;

  // The ELF build id identifies this exact binary, so a cached scan can be trusted only for the
  // same build. It lives in the first page, so a small fault-safe read of the load base is enough.
  constexpr std::size_t kHeaderProbeBytes = 4096;
  std::vector<std::uint8_t> header(std::min(kHeaderProbeBytes, out.regions.front().end -
                                                                 out.regions.front().start));
  const size_t headerRead =
      ReadRegionSafely(out.regions.front().start, header.size(), header.data());
  if (headerRead > 0) {
    ReadElfBuildId(header.data(), headerRead, out.buildId);
  }

  // `RTLD_NOLOAD` only: a signature lookup must never load a library as a side effect. Without a
  // handle the module is still scanned by byte pattern, so the handle is an optimisation (dlsym of
  // an exported name) rather than a requirement.
  out.handle = dlopen(name.c_str(), RTLD_LAZY | RTLD_NOLOAD);
  return true;
}

// The module cache. A cached entry is re-validated by probing that its regions are still mapped, so
// a module that was unmapped or reloaded at different addresses is re-read instead of answering from
// a stale region list.
std::unordered_map<std::string, ModuleInfo> moduleCache;

ModuleInfo getModuleInfoFresh(const std::string &moduleName) {
  {
    std::shared_lock lock(cacheMutex);
    const auto it = moduleCache.find(moduleName);
    if (it != moduleCache.end()) {
      ModuleInfo cached = it->second;
      // Cheap identity re-check: the regions must still be mapped. This costs a few
      // `process_vm_readv` calls and catches a module that was unmapped since it was cached.
      if (RegionsAreMapped(cached)) return cached;
      // Fall through to a fresh read; the stale entry is replaced below.
    }
  }

  ModuleInfo module;
  if (!getModuleInfo(moduleName, module)) return {};

  std::unique_lock lock(cacheMutex);
  moduleCache[moduleName] = module;
  return module;
}

ParsedPattern getCachedPattern(const std::string &signature) {
  {
    std::shared_lock lock(cacheMutex);
    const auto it = patternCache.find(signature);
    if (it != patternCache.end()) return it->second;
  }

  ParsedPattern pattern = parsePattern(signature);
  std::unique_lock lock(cacheMutex);
  const auto [it, inserted] = patternCache.emplace(signature, pattern);
  return inserted ? pattern : it->second;
}

bool matchesPatternAt(const uint8_t *data, const ParsedPattern &pattern) {
  for (const size_t index : pattern.checkIndices) {
    if (index >= pattern.anchorIndex &&
        index < pattern.anchorIndex + pattern.anchorSize) {
      continue;
    }
    if (!matches(pattern.bytes[index], data[index])) return false;
  }
  return true;
}

bool matchesAnchorAt(const uint8_t *data, size_t regionSize,
                     size_t anchorOffset, const ParsedPattern &pattern) {
  if (anchorOffset + pattern.anchorSize > regionSize) return false;
  for (size_t i = 0; i < pattern.anchorSize; ++i) {
    if (!matches(pattern.bytes[pattern.anchorIndex + i],
                 data[anchorOffset + i])) {
      return false;
    }
  }
  return true;
}

std::vector<CompiledPattern>
compilePatterns(const std::vector<std::string> &signatures,
                std::unordered_map<std::string, uintptr_t> &results) {
  std::vector<CompiledPattern> compiled;
  compiled.reserve(signatures.size());
  for (const auto &signature : signatures) {
    auto pattern = getCachedPattern(signature);
    if (pattern.bytes.empty()) {
      preloaderLogger.warn("invalid signature pattern: {}", signature);
      results[signature] = 0;
      continue;
    }
    selectAnchor(pattern);
    compiled.push_back(CompiledPattern{signature, std::move(pattern)});
  }
  return compiled;
}

std::vector<AnchorNode>
buildAnchorAutomaton(const std::vector<CompiledPattern> &patterns,
                     const std::vector<bool> &active,
                     std::vector<size_t> &maskedPatterns) {
  std::vector<AnchorNode> nodes(1);

  for (size_t index = 0; index < patterns.size(); ++index) {
    if (!active[index]) continue;
    const auto &pattern = patterns[index].pattern;
    bool exact = true;
    for (size_t i = 0; i < pattern.anchorSize; ++i) {
      if (!isExactByte(pattern.bytes[pattern.anchorIndex + i])) {
        exact = false;
        break;
      }
    }
    if (!exact) {
      maskedPatterns.push_back(index);
      continue;
    }

    int state = 0;
    for (size_t i = 0; i < pattern.anchorSize; ++i) {
      const uint8_t value = pattern.bytes[pattern.anchorIndex + i].value;
      int next = nodes[state].next[value];
      if (next == -1) {
        next = static_cast<int>(nodes.size());
        nodes[state].next[value] = next;
        nodes.emplace_back();
      }
      state = next;
    }
    nodes[state].outputs.push_back(index);
  }

  std::queue<int> queue;
  for (size_t value = 0; value < 256; ++value) {
    int &next = nodes[0].next[value];
    if (next == -1) {
      next = 0;
    } else {
      nodes[next].failure = 0;
      queue.push(next);
    }
  }

  while (!queue.empty()) {
    const int state = queue.front();
    queue.pop();
    for (size_t value = 0; value < 256; ++value) {
      int &next = nodes[state].next[value];
      if (next == -1) {
        next = nodes[nodes[state].failure].next[value];
        continue;
      }

      const int failure = nodes[nodes[state].failure].next[value];
      nodes[next].failure = failure;
      const auto &outputs = nodes[failure].outputs;
      nodes[next].outputs.insert(nodes[next].outputs.end(), outputs.begin(),
                                 outputs.end());
      queue.push(next);
    }
  }

  return nodes;
}

// Reads a region into a local buffer through `process_vm_readv`, which faults on the reader's side
// rather than the target's: a region that unmaps between the maps parse and the read makes the
// syscall return short, and the caller skips it instead of dereferencing freed memory. The returned
// count is the number of bytes actually read.
size_t ReadRegionSafely(uintptr_t start, size_t size, uint8_t *out) {
  if (size == 0) return 0;
  iovec local{out, size};
  iovec remote{reinterpret_cast<void *>(start), size};
  const ssize_t read = ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0);
  return read > 0 ? static_cast<size_t>(read) : 0;
}

void scanCompiledPatterns(const std::vector<MemoryRegion> &regions,
                          const std::vector<CompiledPattern> &patterns,
                          std::unordered_map<std::string, uintptr_t> &results) {
  if (patterns.empty()) return;

  std::vector<uintptr_t> found(patterns.size(), 0);
  std::vector<bool> active(patterns.size(), true);
  size_t unresolved = patterns.size();

  if (regions.empty()) {
    unresolved = 0;
  } else {
    for (size_t i = 0; i < patterns.size(); ++i) {
      if (patterns[i].pattern.checkIndices.empty()) {
        found[i] = regions.front().start;
        active[i] = false;
        --unresolved;
      }
    }
  }

  std::vector<size_t> maskedPatterns;
  const auto nodes = buildAnchorAutomaton(patterns, active, maskedPatterns);

  auto tryMatch = [&](uintptr_t bufferBase, const uint8_t *data, size_t bufferSize,
                      size_t anchorOffset, size_t patternIndex) {
    if (!active[patternIndex]) return;
    const auto &pattern = patterns[patternIndex].pattern;
    if (bufferSize < pattern.bytes.size() || anchorOffset < pattern.anchorIndex ||
        !matchesAnchorAt(data, bufferSize, anchorOffset, pattern)) {
      return;
    }

    const size_t candidateOffset = anchorOffset - pattern.anchorIndex;
    if (candidateOffset > bufferSize - pattern.bytes.size() ||
        !matchesPatternAt(data + candidateOffset, pattern)) {
      return;
    }

    found[patternIndex] = bufferBase + candidateOffset;
    active[patternIndex] = false;
    --unresolved;
  };

  // A region can be large (libminecraftpe's `.text` is ~220 MB), so it is read in windows. Each
  // window is copied out with a fault-safe read; consecutive windows overlap by the longest pattern
  // so a match that straddles a window boundary is still found.
  size_t longestPattern = 1;
  for (const auto &pattern : patterns) {
    longestPattern = std::max(longestPattern, pattern.pattern.bytes.size());
  }
  const size_t overlap = longestPattern > 1 ? longestPattern - 1 : 0;
  constexpr size_t kWindowBytes = 1U << 20;
  const size_t step = kWindowBytes > overlap ? kWindowBytes - overlap : kWindowBytes;
  std::vector<uint8_t> buffer(kWindowBytes);

  for (const auto &region : regions) {
    if (unresolved == 0) break;
    const size_t regionSize = region.end - region.start;

    for (size_t windowStart = 0; windowStart < regionSize && unresolved != 0;
         windowStart += step) {
      const size_t windowSize = std::min(kWindowBytes, regionSize - windowStart);
      const size_t read = ReadRegionSafely(region.start + windowStart, windowSize, buffer.data());
      if (read == 0) {
        // The region unmapped or is unreadable; skip it rather than dereference it.
        preloaderLogger.debug("signature scan: skipped an unreadable region at {:x}",
                              static_cast<unsigned long long>(region.start));
        continue;
      }
      const uintptr_t bufferBase = region.start + windowStart;
      const auto *data = buffer.data();
      int state = 0;

      for (size_t offset = 0; offset < read && unresolved != 0; ++offset) {
        state = nodes[state].next[data[offset]];
        for (const size_t patternIndex : nodes[state].outputs) {
          const auto anchorSize = patterns[patternIndex].pattern.anchorSize;
          if (offset + 1 >= anchorSize) {
            tryMatch(bufferBase, data, read, offset + 1 - anchorSize, patternIndex);
          }
        }

        for (const size_t patternIndex : maskedPatterns) {
          if (!active[patternIndex]) continue;
          const auto &pattern = patterns[patternIndex].pattern;
          if (matches(pattern.bytes[pattern.anchorIndex], data[offset])) {
            tryMatch(bufferBase, data, read, offset, patternIndex);
          }
        }
      }
    }
  }

  for (size_t i = 0; i < patterns.size(); ++i) {
    results[patterns[i].signature] = found[i];
  }
}

// The cache key includes the module's ELF build id, so a scan result is only reused for the exact
// binary it was derived from: a game update relinks libminecraftpe and the offsets are re-derived
// rather than answered from the previous build. Without a build id the module name is the key, which
// is the pre-existing behaviour.
std::string makeSignatureCacheKey(std::string_view moduleName, std::string_view buildId,
                                  std::string_view signature) {
  std::string key;
  key.reserve(moduleName.size() + buildId.size() + signature.size() + 4);
  key.append(moduleName).append("::").append(buildId).append("::").append(signature);
  return key;
}

}

std::unordered_map<std::string, uintptr_t>
resolveSignatures(std::span<const std::string> signatures,
                  std::string_view moduleName) {
  std::unordered_map<std::string, uintptr_t> results;
  std::vector<std::string> pending;
  std::unordered_map<std::string, size_t> pendingLookup;

  if (moduleName.empty()) {
    for (const auto &signature : signatures) results[signature] = 0;
    return results;
  }

  // The module is resolved first, because the cache key carries its build id: a scan result is only
  // reusable for the exact binary it was derived from, so a game update cannot answer from the
  // previous build's offsets.
  const ModuleInfo module = getModuleInfoFresh(std::string(moduleName));

  {
    std::shared_lock lock(cacheMutex);
    for (const auto &signature : signatures) {
      const auto key = makeSignatureCacheKey(moduleName, module.buildId, signature);
      const auto cached = sigCache.find(key);
      if (cached != sigCache.end()) {
        results[signature] = cached->second;
      } else if (pendingLookup.emplace(signature, pending.size()).second) {
        pending.push_back(signature);
      }
    }
  }

  if (pending.empty()) return results;

  if (module.regions.empty()) {
    for (const auto &signature : pending) results[signature] = 0;
  } else {
    std::vector<std::string> patterns;
    patterns.reserve(pending.size());
    for (const auto &signature : pending) {
      if (module.handle) {
        if (void *symbol = dlsym(module.handle, signature.c_str())) {
          results[signature] = reinterpret_cast<uintptr_t>(symbol);
          continue;
        }
      }
      patterns.push_back(signature);
    }

    if (!patterns.empty()) {
      const auto compiled = compilePatterns(patterns, results);
      // Code signatures are scanned only in the module's executable regions. A data section is
      // never the target of a byte-pattern code signature, and including writable mappings both
      // wastes the scan and can resolve a pattern to a mutable buffer that happens to hold the
      // bytes.
      const std::vector<MemoryRegion> &scanRegions =
          module.execRegions.empty() ? module.regions : module.execRegions;
      scanCompiledPatterns(scanRegions, compiled, results);
    }
  }

  std::unique_lock lock(cacheMutex);
  for (const auto &signature : pending) {
    sigCache[makeSignatureCacheKey(moduleName, module.buildId, signature)] = results[signature];
  }
  return results;
}

uintptr_t resolveSignature(std::string_view signature,
                           std::string_view moduleName) {
  std::vector<std::string> signatures{std::string(signature)};
  const auto results = resolveSignatures(signatures, moduleName);
  const auto it = results.find(std::string(signature));
  return it == results.end() ? 0 : it->second;
}

bool isExecutableAddress(uintptr_t address, std::string_view moduleName) {
  if (address == 0 || moduleName.empty()) return false;
  const ModuleInfo module = getModuleInfoFresh(std::string(moduleName));
  for (const auto &region : module.execRegions) {
    if (address >= region.start && address < region.end) return true;
  }
  return false;
}

}
