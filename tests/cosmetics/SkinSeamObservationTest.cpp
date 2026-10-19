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

  // Joins observed but no skin image built: the drop is at or before the image-loader step.
  expect(ClassifySkinSeam(true, 1, false, 0, false) == SkinSeamStage::JoinObservedNoImage,
         "one join, no image -> drop at/before step 2");
  expect(ClassifySkinSeam(true, 1000, false, 5, false) == SkinSeamStage::JoinObservedNoImage,
         "many joins, no captured image -> drop at/before step 2");

  // Joins observed and a skin image was built: the drop, if any, is downstream.
  expect(ClassifySkinSeam(true, 1000, true, 5, true) == SkinSeamStage::JoinObservedWithImage,
         "joins + captured image -> drop downstream");
  expect(ClassifySkinSeam(true, 1, false, 0, true) == SkinSeamStage::JoinObservedWithImage,
         "a captured image without the texture hook still proves step 2");

  // Every stage has a distinct, non-empty line, and the observed lines state the limitation rather
  // than claiming the skin was sent.
  const char *absent = DescribeSkinSeam(SkinSeamStage::JoinHookAbsent);
  const char *idle = DescribeSkinSeam(SkinSeamStage::JoinHookIdle);
  const char *noImage = DescribeSkinSeam(SkinSeamStage::JoinObservedNoImage);
  const char *withImage = DescribeSkinSeam(SkinSeamStage::JoinObservedWithImage);
  expect(absent != nullptr && absent[0] != '\0', "absent line is non-empty");
  expect(idle != nullptr && idle[0] != '\0', "idle line is non-empty");
  expect(noImage != nullptr && noImage[0] != '\0', "no-image line is non-empty");
  expect(withImage != nullptr && withImage[0] != '\0', "with-image line is non-empty");
  expect(absent != idle && idle != noImage && noImage != withImage && absent != withImage,
         "lines are distinct");
  {
    std::string line(noImage);
    expect(line.find("step 2") != std::string::npos,
           "the no-image line names the image-loader step");
  }
  {
    std::string line(withImage);
    expect(line.find("downstream") != std::string::npos,
           "the with-image line places the drop downstream");
  }
  {
    std::string line(absent);
    expect(line.find("not installed") != std::string::npos,
           "the absent line names the missing hook");
  }

  std::printf("SkinSeamObservationTest: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
