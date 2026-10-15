/**
 * @file ModuleMapTest.cpp
 * @brief Host test for the module-mapping counter.
 *
 * A second copy of libpreloader.so has its own empty registry, so Java publishes to one and the hook
 * draws from another — cosmetics silently never appear. The count of mappings is the diagnostics
 * proof; the parse is pinned here with a synthetic /proc/self/maps body.
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "pl/runtime/ModuleMap.hpp"

using pl::runtime::CountModuleMappingsIn;

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
  // A single load: the executable segment is mapped once, the data segments alongside it.
  const std::string oneLoad =
      "7a00000000-7a00001000 r-xp 00000000 fd:00 1 /data/app/x/lib/arm64/libpreloader.so\n"
      "7a00001000-7a00002000 r--p 00001000 fd:00 1 /data/app/x/lib/arm64/libpreloader.so\n"
      "7a00002000-7a00003000 rw-p 00002000 fd:00 1 /data/app/x/lib/arm64/libpreloader.so\n";
  expect(CountModuleMappingsIn(oneLoad, "libpreloader.so") == 1,
         "a single load counts one copy, not one per segment");

  // The same module loaded twice: two executable segments at different addresses.
  const std::string twoLoads =
      "7a00000000-7a00001000 r-xp 00000000 fd:00 1 /data/app/x/lib/arm64/libpreloader.so\n"
      "7b00000000-7b00001000 r-xp 00000000 fd:00 1 /data/app/y/lib/arm64/libpreloader.so\n";
  expect(CountModuleMappingsIn(twoLoads, "libpreloader.so") == 2, "two distinct loads counted");

  // A similar-but-different name must not match.
  const std::string decoy =
      "7a00000000-7a00001000 r-xp 00000000 fd:00 1 /data/app/x/lib/arm64/libpreloader_extra.so\n";
  expect(CountModuleMappingsIn(decoy, "libpreloader.so") == 0, "a longer name does not match");

  // An unrelated module does not match.
  expect(CountModuleMappingsIn(oneLoad, "libminecraftpe.so") == 0, "a different module is not counted");

  // Anonymous mappings (no path) are ignored.
  const std::string anon =
      "7a00000000-7a00001000 r-xp 00000000 00:00 0 [anon:libpreloader]\n";
  expect(CountModuleMappingsIn(anon, "libpreloader.so") == 0, "an anon mapping with a similar tag is ignored");

  expect(CountModuleMappingsIn("", "libpreloader.so") == 0, "empty text counts zero");
  expect(CountModuleMappingsIn(oneLoad, "") == 0, "empty module name counts zero");

  std::printf("ModuleMapTest: %d checks passed\n", checks);
  return 0;
}
