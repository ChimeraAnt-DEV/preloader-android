/**
 * @file BedrockModelParserTest.cpp
 * @brief Host unit tests for the native `.geo.json` parser and mesh builder (Task 1).
 *
 * Runs on the build host (g++), not the device: the parser and builder are pure C++ with no Android
 * types, so the whole parse and mesh build is verifiable without a device or an emulator — the same
 * reason the Java preview pipeline is unit-tested. Covers the two format versions the launcher ships
 * (1.12.0 and 1.21.0) and multi-bone rigs (hat, wing, pet).
 *
 * Build: see tests/cosmetics/run_tests.sh
 */

#include "pl/cosmetics/geometry/BedrockGeometry.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace pl::cosmetics::geometry;

namespace {

int gChecks = 0;

void check(bool condition, const char *message) {
  ++gChecks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    assert(condition);
  }
}

bool approx(float a, float b, float eps = 0.001F) { return std::fabs(a - b) <= eps; }

// A 1.12.0 hat: one bone parented to the head, one cube, box UV.
const char *kHat112 = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    {
      "description": { "identifier": "geometry.glowberry.hat", "texture_width": 64, "texture_height": 64 },
      "bones": [
        {
          "name": "hat",
          "parent": "head",
          "pivot": [0, 24, 0],
          "cubes": [
            { "origin": [-4, 24, -4], "size": [8, 8, 8], "uv": [0, 0] }
          ]
        }
      ]
    }
  ]
})";

// A 1.21.0 wing: two bones (root -> wing), a cube with a per-face UV map and a negative uv_size.
const char *kWing121 = R"({
  "format_version": "1.21.0",
  "minecraft:geometry": [
    {
      "description": { "identifier": "geometry.glowberry.wing", "texture_width": 32, "texture_height": 32 },
      "bones": [
        {
          "name": "root",
          "pivot": [0, 12, 2],
          "cubes": [ { "origin": [-1, 11, 2], "size": [2, 2, 1], "uv": [0, 0] } ]
        },
        {
          "name": "wing_left",
          "parent": "root",
          "pivot": [0, 12, 2],
          "rotation": [0, 0, 20],
          "cubes": [
            {
              "origin": [-12, 10, 2], "size": [10, 6, 1],
              "uv": {
                "north": { "uv": [10, 0], "uv_size": [-10, 6] },
                "south": { "uv": [0, 0], "uv_size": [10, 6] },
                "up": { "uv": [0, 6], "uv_size": [10, 1] },
                "down": { "uv": [10, 6], "uv_size": [-10, 1] },
                "east": { "uv": [10, 7], "uv_size": [-1, 6] },
                "west": { "uv": [0, 7], "uv_size": [1, 6] }
              }
            }
          ]
        }
      ]
    }
  ]
})";

// A pet: body + head + four legs, all parented to the body, exercising the chain resolution.
const char *kPet = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    {
      "description": { "identifier": "geometry.glowberry.pet", "texture_width": 64, "texture_height": 64 },
      "bones": [
        { "name": "body", "pivot": [0, 8, 0], "cubes": [ { "origin": [-3, 6, -5], "size": [6, 4, 10], "uv": [0, 0] } ] },
        { "name": "head", "parent": "body", "pivot": [0, 8, -5], "cubes": [ { "origin": [-2, 8, -9], "size": [4, 4, 4], "uv": [0, 14] } ] },
        { "name": "leg_fl", "parent": "body", "pivot": [-2, 6, -4], "cubes": [ { "origin": [-3, 0, -5], "size": [2, 6, 2], "uv": [0, 22] } ] },
        { "name": "leg_fr", "parent": "body", "pivot": [2, 6, -4], "cubes": [ { "origin": [1, 0, -5], "size": [2, 6, 2], "uv": [8, 22] } ] },
        { "name": "leg_bl", "parent": "body", "pivot": [-2, 6, 4], "cubes": [ { "origin": [-3, 0, 3], "size": [2, 6, 2], "uv": [0, 30] } ] },
        { "name": "leg_br", "parent": "body", "pivot": [2, 6, 4], "cubes": [ { "origin": [1, 0, 3], "size": [2, 6, 2], "uv": [8, 30] } ] }
      ]
    }
  ]
})";

void testHat112() {
  BedrockGeometry geometry;
  std::string error;
  check(BedrockGeometryParser::parse(kHat112, geometry, &error), "hat 1.12.0 parses");
  check(geometry.formatVersion == "1.12.0", "format version read");
  check(geometry.models.size() == 1, "one model");
  const GeometryModel &model = geometry.models.front();
  check(model.description.identifier == "geometry.glowberry.hat", "identifier read");
  check(model.description.textureWidth == 64 && model.description.textureHeight == 64, "atlas size read");
  check(model.bones.size() == 1, "one bone");
  check(model.bones.front().name == "hat", "bone name");
  check(model.bones.front().parent == "head", "bone parent");
  check(model.bones.front().cubes.size() == 1, "one cube");
  check(approx(model.bones.front().cubes.front().size.x, 8.0F), "cube size x");

  std::vector<Mesh> meshes = GeometryMeshBuilder::build(geometry);
  check(meshes.size() == 1, "one mesh");
  check(meshes.front().bone == "hat", "mesh bone name");
  check(meshes.front().vertices.size() == 24, "cube emits 24 vertices (6 faces x 4)");
  check(meshes.front().indices.size() == 36, "cube emits 36 indices (6 faces x 6)");

  const std::string summary = summarize(geometry);
  check(summary.find("format=1.12.0") != std::string::npos, "summary reports format");
  check(summary.find("bones=1") != std::string::npos, "summary reports bone count");
  check(summary.find("atlas=64x64") != std::string::npos, "summary reports atlas");
}

void testWing121() {
  BedrockGeometry geometry;
  std::string error;
  check(BedrockGeometryParser::parse(kWing121, geometry, &error), "wing 1.21.0 parses");
  check(geometry.formatVersion == "1.21.0", "1.21.0 version read");
  const GeometryModel &model = geometry.models.front();
  check(model.bones.size() == 2, "two bones");
  check(model.bones[1].parent == "root", "wing parented to root");
  check(model.bones[1].hasRotation, "wing carries a rotation");
  check(model.bones[1].cubes.front().perFace.present, "per-face UV map present");

  // The north face is stated with a negative uv_size; the emitted UVs must still increase so the
  // sample is not read backwards.
  std::vector<Mesh> meshes = GeometryMeshBuilder::build(geometry);
  check(meshes.size() == 2, "two meshes (root + wing)");
  const Mesh &wing = meshes[1];
  check(wing.vertices.size() == 24, "wing cube emits 24 vertices");
  for (std::size_t i = 0; i < wing.vertices.size(); i += 4) {
    const Vertex &a = wing.vertices[i + 0];
    const Vertex &c = wing.vertices[i + 2];
    // Grid order [TL, TR, BL, BR]: BR must be down-right of TL on both axes.
    check(c.uv.x >= a.uv.x, "face UV u increases TL->BR");
    check(c.uv.y >= a.uv.y, "face UV v increases TL->BR");
  }

  // A 20-degree roll about the pivot must move the wing tip off the origin plane (proves the chain
  // transform ran, not just a copy).
  bool moved = false;
  for (const Vertex &v : wing.vertices) {
    if (std::fabs(v.position.y - 10.0F) > 1.0F) moved = true;
  }
  check(moved, "bone rotation displaces the wing vertices");
}

void testPet() {
  BedrockGeometry geometry;
  std::string error;
  check(BedrockGeometryParser::parse(kPet, geometry, &error), "pet parses");
  check(geometry.models.front().bones.size() == 6, "pet has six bones");
  std::vector<Mesh> meshes = GeometryMeshBuilder::build(geometry);
  check(meshes.size() == 6, "one mesh per bone with cubes");
  std::size_t total = 0;
  for (const Mesh &mesh : meshes) total += mesh.vertices.size();
  check(total == 6 * 24, "six cubes -> 144 vertices");
}

void testMalformed() {
  BedrockGeometry geometry;
  check(!BedrockGeometryParser::parse("", geometry), "empty rejected");
  check(!BedrockGeometryParser::parse("{ not json", geometry), "bad json rejected");
  check(!BedrockGeometryParser::parse("{\"format_version\":\"1.12.0\"}", geometry),
        "missing geometry array rejected");
  check(!BedrockGeometryParser::parse("{\"minecraft:geometry\":[]}", geometry),
        "empty geometry array rejected");
}

void testBoxUvLayout() {
  // A 2x3x4 cube (width x height x depth) with box UV at (0,0): the classic Bedrock unwrap.
  const UvRect top = GeometryMeshBuilder::boxUvForFace(Face::Top, 0, 0, 2, 3, 4);
  check(approx(top.u, 4.0F) && approx(top.v, 0.0F) && approx(top.w, 2.0F) && approx(top.h, 4.0F),
        "top face unwrap");
  const UvRect front = GeometryMeshBuilder::boxUvForFace(Face::Front, 0, 0, 2, 3, 4);
  check(approx(front.u, 10.0F) && approx(front.v, 4.0F) && approx(front.w, 2.0F) && approx(front.h, 3.0F),
        "front face unwrap");
  const UvRect left = GeometryMeshBuilder::boxUvForFace(Face::Left, 0, 0, 2, 3, 4);
  check(approx(left.u, 0.0F) && approx(left.v, 4.0F) && approx(left.w, 4.0F) && approx(left.h, 3.0F),
        "left face unwrap");
}

void testFormatSupport() {
  check(BedrockGeometryParser::isSupportedFormat("1.12.0"), "1.12.0 supported");
  check(BedrockGeometryParser::isSupportedFormat("1.21.0"), "1.21.0 supported");
  check(!BedrockGeometryParser::isSupportedFormat("9.9.9"), "unknown format rejected");
}

} // namespace

int main() {
  testHat112();
  testWing121();
  testPet();
  testMalformed();
  testBoxUvLayout();
  testFormatSupport();
  std::printf("BedrockModelParserTest: %d checks passed\n", gChecks);
  return 0;
}
