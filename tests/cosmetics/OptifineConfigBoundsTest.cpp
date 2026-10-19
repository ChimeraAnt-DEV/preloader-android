// Host test for the Optifine config parser's bounds (item 20). Pure C++, no device.
#include <cstdio>
#include <string>

#include "pl/runtime/OptifineConfig.h"

using pl::runtime::kOptifineMaxBlobBytes;
using pl::runtime::kOptifineMaxKeyLength;
using pl::runtime::kOptifineMaxKeys;
using pl::runtime::OptifineDecision;
using pl::runtime::ParseOptifineConfig;

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
  // A normal blob still parses exactly as before.
  {
    const std::string blob =
        "master=1\nenabled.entity_culling=1\nenabled.particle_culling=1\ncrash.particle_culling=2\n";
    const auto config = ParseOptifineConfig(blob);
    expect(config.master, "master parses");
    expect(config.enabled[4] && config.enabled[5], "items parse");
    expect(config.crashCount[5] == 2, "crash count parses");
  }

  // A blob past the byte cap is parsed up to the cap and no further, not rejected. The oversize
  // line after the cap must not be stored.
  {
    std::string blob = "master=1\n";
    blob.append(kOptifineMaxBlobBytes, 'x');  // filler with no '=' to keep the map small
    blob += "\nenabled.entity_culling=1\n";
    const auto config = ParseOptifineConfig(blob);
    expect(config.master, "the pre-cap keys still parse");
    expect(!config.enabled[4], "a key past the byte cap is ignored, not rejected");
  }

  // More keys than the cap: the map stops growing, the first keys win.
  {
    std::string blob = "master=1\n";
    for (std::size_t i = 0; i < kOptifineMaxKeys * 2; ++i) {
      blob += "enabled.entity_culling=" + std::to_string(i) + "\n";
    }
    const auto config = ParseOptifineConfig(blob);
    expect(config.master, "master survives the key flood");
  }

  // An over-long key is skipped; a normal key after it still parses.
  {
    std::string blob = std::string(kOptifineMaxKeyLength + 32, 'k') + "=1\nmaster=1\n";
    const auto config = ParseOptifineConfig(blob);
    expect(config.master, "a key after an over-long key still parses");
  }

  // An over-long value is truncated and a value that is not a recognised token reads as off
  // (fail-closed), rather than being treated as truthy because it is long.
  {
    std::string blob = "master=1\nenabled.entity_culling=" + std::string(4096, 'x') + "\n";
    const auto config = ParseOptifineConfig(blob);
    expect(!config.enabled[4], "an over-long junk value reads as off");
  }

  // A long but whitespace-padded valid value trims to the token and still reads as on, so the cap
  // does not break a value a real launcher sends.
  {
    std::string blob = "master=1\nenabled.entity_culling=1" + std::string(4096, ' ') + "\n";
    const auto config = ParseOptifineConfig(blob);
    expect(config.enabled[4], "a padded '1' still reads as on");
    expect(pl::runtime::DecideOptifineItem(config, 4) == OptifineDecision::Apply,
           "the decision is unaffected by value padding");
  }

  // Empty and malformed blobs are still safe.
  {
    const auto empty = ParseOptifineConfig("");
    expect(!empty.master, "an empty blob reads as off");
    const auto junk = ParseOptifineConfig("=no key\nno equals\nmaster=1\n");
    expect(junk.master, "a keyless line does not break the parse");
  }

  std::printf("OptifineConfigBoundsTest: %d checks passed\n", checks);
  return failures == 0 ? 0 : 1;
}
