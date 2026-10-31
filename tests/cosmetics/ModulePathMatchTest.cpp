/**
 * @file ModulePathMatchTest.cpp
 * @brief Host test for the `/proc/self/maps` line parse and basename match.
 *
 * Resolving a signature collects the module's regions by matching the maps path column. The obvious
 * `strstr(line, moduleName)` also matches a path that merely contains the name, so the scanner ends
 * up reading an unrelated library. The match here is on the exact basename, pinned below.
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "pl/memory/ModulePathMatch.hpp"

using pl::memory::MapsLine;
using pl::memory::MapsPathNamesModule;
using pl::memory::MapsPermsExecutable;
using pl::memory::MapsPermsReadable;
using pl::memory::ParseMapsLine;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

} // namespace

int main() {
  MapsLine line{};
  expect(ParseMapsLine(
             "7a00000000-7a00001000 r-xp 00000000 fd:00 1 /data/app/x/lib/arm64/libminecraftpe.so",
             line),
         "a full maps line parses");
  expect(line.start == 0x7a00000000ULL && line.end == 0x7a00001000ULL, "the address range parses");
  expect(std::string(line.perms) == "r-xp", "the permission string parses");
  expect(line.path == "/data/app/x/lib/arm64/libminecraftpe.so", "the path parses");

  // A missing or malformed head is rejected.
  expect(!ParseMapsLine("", line), "an empty line is rejected");
  expect(!ParseMapsLine("not a maps line", line), "a non-maps line is rejected");
  expect(!ParseMapsLine("7a00000000 7a00001000 r-xp /x.so", line), "a missing dash is rejected");
  expect(!ParseMapsLine("7a00001000-7a00000000 r-xp /x.so", line), "an inverted range is rejected");
  expect(!ParseMapsLine("zzzz-7a00001000 r-xp /x.so", line), "a non-hex address is rejected");

  // An anonymous mapping has no path and never matches a module.
  MapsLine anon{};
  expect(ParseMapsLine("7a00000000-7a00001000 rw-p 00000000 00:00 0", anon), "an anon line parses");
  expect(anon.path.empty(), "an anon line has an empty path");
  expect(!MapsPathNamesModule(anon.path, "libminecraftpe.so"), "an empty path never matches");

  // The basename match is exact.
  expect(MapsPathNamesModule("/data/app/x/lib/arm64/libminecraftpe.so", "libminecraftpe.so"),
         "an exact basename matches");
  expect(!MapsPathNamesModule("/data/app/x/lib/arm64/libminecraftpe_extra.so", "libminecraftpe.so"),
         "a longer basename does not match");
  expect(!MapsPathNamesModule("/data/libminecraftpe.so.1/lib/other.so", "libminecraftpe.so"),
         "a directory segment does not match");
  expect(!MapsPathNamesModule("/data/app/x/lib/arm64/libminecraftpe.so", "libc.so"),
         "a different name does not match");
  expect(MapsPathNamesModule("libpreloader.so", "libpreloader.so"),
         "a bare basename matches");
  expect(!MapsPathNamesModule("/x/libpreloader.so", ""), "an empty module name never matches");
  expect(!MapsPathNamesModule("", "libpreloader.so"), "an empty path never matches");

  // Permission helpers.
  expect(MapsPermsExecutable("r-xp"), "r-xp is executable");
  expect(!MapsPermsExecutable("rw-p"), "rw-p is not executable");
  expect(!MapsPermsExecutable("r-"), "a short permission string is not executable");
  expect(MapsPermsReadable("r-xp"), "r-xp is readable");
  expect(!MapsPermsReadable("-w-p"), "a non-readable mapping is not readable");

  // The tag form of an anonymous mapping keeps its text but never matches a module name.
  MapsLine tagged{};
  expect(ParseMapsLine("7a00000000-7a00001000 rw-p 00000000 00:00 0 [anon:libpreloader]",
                       tagged),
         "a tagged anon line parses");
  expect(tagged.path == "[anon:libpreloader]", "the tag is kept verbatim");
  expect(!MapsPathNamesModule(tagged.path, "libpreloader.so"), "a tag never matches a module");

  std::printf("ModulePathMatchTest: %d checks passed\n", checks);
  return 0;
}
