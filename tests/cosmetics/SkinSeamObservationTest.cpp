// Host test for the skin-seam classification (no device, no mocks).
#include <cstdio>
#include <string>

#include "pl/cosmetics/SkinSeamObservation.hpp"

using pl::cosmetics::ClassifySkinSeam;
using pl::cosmetics::DescribeSkinSeam;
using pl::cosmetics::SkinSeamStage;

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
  // No hook: the client side is unproven, whatever the other counters say.
  expect(ClassifySkinSeam(false, 0, false, 0, false) == SkinSeamStage::JoinHookAbsent,
         "no join hook -> absent");
  expect(ClassifySkinSeam(false, 5, true, 100, true) == SkinSeamStage::JoinHookAbsent,
         "no join hook wins over every other counter");

  // Hook installed but idle: proves the seam exists, not that the traffic does.
  expect(ClassifySkinSeam(true, 0, true, 100, true) == SkinSeamStage::JoinHookIdle,
         "installed with no calls -> idle");

  // Joins observed: the furthest this build can prove.
  expect(ClassifySkinSeam(true, 1, false, 0, false) == SkinSeamStage::JoinObserved,
         "one join -> observed");
  expect(ClassifySkinSeam(true, 1000, true, 5, true) == SkinSeamStage::JoinObserved,
         "many joins -> observed");

  // Every stage has a distinct, non-empty line, and the observed line states the limitation rather
  // than claiming the skin was sent.
  const char *absent = DescribeSkinSeam(SkinSeamStage::JoinHookAbsent);
  const char *idle = DescribeSkinSeam(SkinSeamStage::JoinHookIdle);
  const char *observed = DescribeSkinSeam(SkinSeamStage::JoinObserved);
  expect(absent != nullptr && absent[0] != '\0', "absent line is non-empty");
  expect(idle != nullptr && idle[0] != '\0', "idle line is non-empty");
  expect(observed != nullptr && observed[0] != '\0', "observed line is non-empty");
  expect(absent != idle && idle != observed && absent != observed, "lines are distinct");
  {
    std::string line(observed);
    expect(line.find("unproven") != std::string::npos,
           "the observed line states the payload is unproven");
  }
  {
    std::string line(absent);
    expect(line.find("not installed") != std::string::npos,
           "the absent line names the missing hook");
  }

  std::printf("SkinSeamObservationTest: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
