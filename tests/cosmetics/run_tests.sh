#!/usr/bin/env bash
# Host test runner for the native cosmetics modules.
#
# The geometry parser, animation solver and socket protocol are pure C++ with no Android types, so
# they are verified on the build host rather than the device. This is the same reason the Java
# preview pipeline is unit-tested: a parse or interpolation defect is caught without an emulator.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
nlohmann="${NLOHMANN_JSON_INCLUDE:-}"

if [[ -z "$nlohmann" ]]; then
  # Fall back to the copy Gradle already fetched, if present.
  nlohmann="$(find "$root/../../../.cxx" -path '*nlohmann_json-src/single_include' -type d 2>/dev/null | head -1 || true)"
fi
if [[ -z "$nlohmann" || ! -f "$nlohmann/nlohmann/json.hpp" ]]; then
  echo "nlohmann/json not found; set NLOHMANN_JSON_INCLUDE to its include dir" >&2
  exit 1
fi

cxx="${CXX:-g++}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

compile_run() {
  local name="$1"; shift
  echo "== $name =="
  "$cxx" -std=c++20 -O1 -g \
    -I"$root/include" -I"$nlohmann" \
    "$@" -o "$work/$name"
  "$work/$name"
}

compile_run BedrockModelParserTest \
  "$root/src/pl/cosmetics/geometry/BedrockModelParser.cpp" \
  "$here/BedrockModelParserTest.cpp"

compile_run NativeModelMatrixTest \
  "$root/src/pl/cosmetics/NativeModelMatrix.cpp" \
  "$here/NativeModelMatrixTest.cpp"

compile_run AnimationSolverTest \
  "$root/src/pl/cosmetics/animation/AnimationSolver.cpp" \
  "$here/AnimationSolverTest.cpp"

compile_run CosmeticSocketProtocolTest \
  "$root/src/pl/cosmetics/network/CosmeticSocketProtocol.cpp" \
  "$here/CosmeticSocketProtocolTest.cpp"

echo "all cosmetics host tests passed"
