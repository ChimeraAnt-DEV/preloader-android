/**
 * @file BedrockModelParser.cpp
 * @brief Native `.geo.json` parser and vertex-buffer builder (Task 1).
 *
 * The parse rules and the mesh arithmetic mirror the launcher's Java preview pipeline exactly (see
 * `core.cosmetics.geometry.PreviewMeshModel`): the same rotation order (Z, then Y, then X), the same
 * chain resolution (own transform, then each ancestor), the same box-UV unwrap and the same
 * negative-UV-size mirror rule. Keeping the two in lockstep is the point — a model that looks right
 * in the preview and one the native render seam attaches must be the same geometry, not two
 * interpretations.
 */

#include "pl/cosmetics/geometry/BedrockGeometry.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <numbers>
#include <string>

namespace pl::cosmetics::geometry {
namespace {

using nlohmann::json;

/** @brief Reads one numeric component, tolerating an int or float JSON primitive. */
bool numberOr(const json &value, float &out) {
  if (value.is_number_integer()) {
    out = static_cast<float>(value.get<long long>());
    return true;
  }
  if (value.is_number_float()) {
    out = static_cast<float>(value.get<double>());
    return true;
  }
  return false;
}

/**
 * @brief Reads a numeric array into `len` components.
 *
 * A single number is broadcast to every slot, which is how Bedrock writes a cube whose size is
 * uniform ({@code "size": 4}). Returns false when the field is absent or not numeric, so the caller
 * can fall back rather than treating a missing field as zero.
 */
bool readVec(const json &object, const char *key, int len, float *out) {
  if (!object.contains(key)) return false;
  const json &element = object.at(key);
  if (element.is_array()) {
    for (int i = 0; i < len; ++i) {
      out[i] = 0.0F;
      if (i < static_cast<int>(element.size())) {
        numberOr(element.at(i), out[i]);
      }
    }
    return true;
  }
  float single = 0.0F;
  if (numberOr(element, single)) {
    for (int i = 0; i < len; ++i) out[i] = single;
    return true;
  }
  return false;
}

Vec3f toVec3(const float *values) { return Vec3f{values[0], values[1], values[2]}; }

UvRect parseRect(const json &element) {
  UvRect rect;
  if (!element.is_object()) return rect;
  float uv[2] = {0.0F, 0.0F};
  float size[2] = {0.0F, 0.0F};
  if (!readVec(element, "uv", 2, uv)) return rect;
  if (!readVec(element, "uv_size", 2, size)) return rect;
  rect.u = uv[0];
  rect.v = uv[1];
  rect.w = size[0];
  rect.h = size[1];
  return rect;
}

void parsePerFace(const json &uvObject, UvPerFace &faces) {
  // Key order follows the Bedrock wiki's face names, mapped to our Face enum:
  //   up -> Top, down -> Bottom, west -> Left, east -> Right, south -> Front, north -> Back.
  static const char *keys[kFaceCount] = {"up", "down", "west", "east", "south", "north"};
  for (int i = 0; i < kFaceCount; ++i) {
    if (uvObject.contains(keys[i])) {
      faces.faces[i] = parseRect(uvObject.at(keys[i]));
      faces.present = true;
    }
  }
}

Cube parseCube(const json &object) {
  Cube cube;
  float origin[3] = {0.0F, 0.0F, 0.0F};
  float size[3] = {0.0F, 0.0F, 0.0F};
  if (!readVec(object, "origin", 3, origin)) return cube;
  if (!readVec(object, "size", 3, size)) return cube;
  cube.origin = toVec3(origin);
  cube.size = toVec3(size);

  float rotation[3] = {0.0F, 0.0F, 0.0F};
  if (readVec(object, "rotation", 3, rotation)) {
    cube.rotation = toVec3(rotation);
    cube.hasRotation = true;
  }
  float pivot[3] = {0.0F, 0.0F, 0.0F};
  if (readVec(object, "pivot", 3, pivot)) {
    cube.pivot = toVec3(pivot);
  }
  float inflate = 0.0F;
  if (numberOr(object.value("inflate", json()), inflate)) {
    cube.inflate = inflate;
  }

  if (object.contains("uv") && object.at("uv").is_object()) {
    const json &uvObject = object.at("uv");
    if (uvObject.contains("north") || uvObject.contains("up") || uvObject.contains("down") ||
        uvObject.contains("east") || uvObject.contains("west") || uvObject.contains("south")) {
      parsePerFace(uvObject, cube.perFace);
    } else {
      float uv[2] = {0.0F, 0.0F};
      float sizeUv[2] = {0.0F, 0.0F};
      if (readVec(uvObject, "uv", 2, uv) && readVec(uvObject, "uv_size", 2, sizeUv)) {
        cube.uv = Vec2f{uv[0], uv[1]};
        cube.uvSize = Vec2f{sizeUv[0], sizeUv[1]};
        cube.hasBoxUv = true;
      }
    }
  } else if (object.contains("uv") && object.at("uv").is_array()) {
    // 1.8 form: the cube's own "uv" is the box-UV origin.
    float uv[2] = {0.0F, 0.0F};
    if (readVec(object, "uv", 2, uv)) {
      cube.uv = Vec2f{uv[0], uv[1]};
      cube.hasBoxUv = true;
    }
  }
  return cube;
}

Bone parseBone(const json &object) {
  Bone bone;
  bone.name = object.value("name", std::string());
  bone.parent = object.value("parent", std::string());
  float pivot[3] = {0.0F, 0.0F, 0.0F};
  if (readVec(object, "pivot", 3, pivot)) {
    bone.pivot = toVec3(pivot);
    bone.hasPivot = true;
  }
  float rotation[3] = {0.0F, 0.0F, 0.0F};
  if (readVec(object, "rotation", 3, rotation)) {
    bone.rotation = toVec3(rotation);
    bone.hasRotation = true;
  }
  if (object.contains("cubes") && object.at("cubes").is_array()) {
    for (const json &cubeJson : object.at("cubes")) {
      if (!cubeJson.is_object()) continue;
      Cube cube = parseCube(cubeJson);
      // A cube with no origin/size parses to a zero box; treat a zero size as absent.
      if (cube.size.x == 0.0F && cube.size.y == 0.0F && cube.size.z == 0.0F) continue;
      bone.cubes.push_back(cube);
    }
  }
  return bone;
}

/** @brief Rotation applied by Blockbench: roll (Z), then yaw (Y), then pitch (X), all degrees. */
Vec3f rotateVector(const Vec3f &v, const Vec3f &rotationDegrees) {
  if (rotationDegrees.x == 0.0F && rotationDegrees.y == 0.0F && rotationDegrees.z == 0.0F) {
    return v;
  }
  constexpr double kDegToRad = std::numbers::pi / 180.0;
  const double ax = rotationDegrees.x * kDegToRad;
  const double ay = rotationDegrees.y * kDegToRad;
  const double az = rotationDegrees.z * kDegToRad;

  const double x1 = v.x * std::cos(az) - v.y * std::sin(az);
  const double y1 = v.x * std::sin(az) + v.y * std::cos(az);
  const double z1 = v.z;
  const double x2 = x1 * std::cos(ay) + z1 * std::sin(ay);
  const double z2 = -x1 * std::sin(ay) + z1 * std::cos(ay);
  const double y2 = y1;
  const double y3 = y2 * std::cos(ax) - z2 * std::sin(ax);
  const double z3 = y2 * std::sin(ax) + z2 * std::cos(ax);
  return Vec3f{static_cast<float>(x2), static_cast<float>(y3), static_cast<float>(z3)};
}

Vec3f rotateAbout(const Vec3f &point, const Vec3f &pivot, const Vec3f &rotation) {
  const Vec3f local{point.x - pivot.x, point.y - pivot.y, point.z - pivot.z};
  const Vec3f rotated = rotateVector(local, rotation);
  return Vec3f{rotated.x + pivot.x, rotated.y + pivot.y, rotated.z + pivot.z};
}

/** @brief A bone-to-root transform chain, innermost first. */
struct Transform {
  Vec3f pivot;
  Vec3f rotation;
};

std::vector<Transform> transformChain(const Bone &bone,
                                      const std::vector<const Bone *> &byName,
                                      const std::vector<std::string> &names) {
  std::vector<Transform> chain;
  const Bone *current = &bone;
  int guard = 0;
  while (current != nullptr && guard++ < 64) {
    chain.push_back(Transform{current->hasPivot ? current->pivot : Vec3f{},
                              current->hasRotation ? current->rotation : Vec3f{}});
    const Bone *parent = nullptr;
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (names[i] == current->parent) {
        parent = byName[i];
        break;
      }
    }
    current = parent;
  }
  return chain;
}

Vec3f applyChain(const Vec3f &point, const std::vector<Transform> &chain) {
  Vec3f p = point;
  for (const Transform &transform : chain) {
    p = rotateAbout(p, transform.pivot, transform.rotation);
  }
  return p;
}

/** @brief Cube corner-index table, in the grid order the Java preview uses (index = ix|iy<<1|iz<<2). */
constexpr int kFaceCorners[kFaceCount][4] = {
    {2, 3, 6, 7}, // Top    (+y)
    {4, 5, 0, 1}, // Bottom (-y)
    {6, 2, 4, 0}, // Left   (-x)
    {3, 7, 1, 5}, // Right  (+x)
    {6, 7, 4, 5}, // Front  (+z)
    {3, 2, 1, 0}, // Back   (-z)
};

constexpr float kFaceNormals[kFaceCount][3] = {
    {0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}, {0, 0, 1}, {0, 0, -1},
};

} // namespace

bool BedrockGeometryParser::isSupportedFormat(const std::string &formatVersion) {
  // The launcher ships 1.12.0 (flat cube array) and 1.21.0 (nested/expanded) assets, plus legacy
  // 1.8.0 files whose geometry key differs. All are structurally the same tree with optional
  // fields, so the parser accepts them; anything else is reported as unsupported.
  static const char *kKnown[] = {"1.8.0", "1.10.0", "1.12.0", "1.16.0", "1.21.0"};
  for (const char *known : kKnown) {
    if (formatVersion == known) return true;
  }
  return false;
}

bool BedrockGeometryParser::parse(const std::string &jsonText, BedrockGeometry &out,
                                  std::string *error) {
  auto fail = [&](const char *reason) {
    if (error != nullptr) *error = reason;
    return false;
  };
  if (jsonText.empty()) return fail("empty document");

  json root = json::parse(jsonText, nullptr, false);
  if (root.is_discarded() || !root.is_object()) return fail("not a JSON object");

  BedrockGeometry geometry;
  if (root.contains("format_version")) {
    const json &version = root.at("format_version");
    if (version.is_string()) {
      geometry.formatVersion = version.get<std::string>();
    } else if (version.is_number()) {
      geometry.formatVersion = std::to_string(version.get<double>());
    }
  }

  const json *models = nullptr;
  if (root.contains("minecraft:geometry") && root.at("minecraft:geometry").is_array()) {
    models = &root.at("minecraft:geometry");
  } else if (root.contains("geometry") && root.at("geometry").is_array()) {
    models = &root.at("geometry");
  }
  if (models == nullptr) return fail("no geometry array");

  for (const json &modelJson : *models) {
    if (!modelJson.is_object()) continue;
    GeometryModel model;
    if (modelJson.contains("description") && modelJson.at("description").is_object()) {
      const json &description = modelJson.at("description");
      model.description.identifier = description.value("identifier", std::string());
      model.description.textureWidth = description.value("texture_width", 64);
      model.description.textureHeight = description.value("texture_height", 64);
    }
    if (!modelJson.contains("bones") || !modelJson.at("bones").is_array()) continue;
    for (const json &boneJson : modelJson.at("bones")) {
      if (!boneJson.is_object()) continue;
      Bone bone = parseBone(boneJson);
      if (!bone.name.empty()) model.bones.push_back(std::move(bone));
    }
    if (!model.bones.empty()) geometry.models.push_back(std::move(model));
  }

  if (geometry.models.empty()) return fail("no model with bones");
  out = std::move(geometry);
  return true;
}

std::string summarize(const BedrockGeometry &geometry) {
  std::string summary = "geo format=" + (geometry.formatVersion.empty() ? "?" : geometry.formatVersion);
  summary += " models=" + std::to_string(geometry.models.size());
  for (const GeometryModel &model : geometry.models) {
    summary += " id=" + (model.description.identifier.empty() ? "-" : model.description.identifier);
    summary += " bones=" + std::to_string(model.bones.size());
    std::size_t cubes = 0;
    for (const Bone &bone : model.bones) cubes += bone.cubes.size();
    summary += " cubes=" + std::to_string(cubes);
    summary += " atlas=" + std::to_string(model.description.textureWidth) + "x" +
               std::to_string(model.description.textureHeight);
  }
  return summary;
}

UvRect GeometryMeshBuilder::boxUvForFace(Face face, float u, float v, float width, float height,
                                         float depth) {
  // The Bedrock box-UV unwrap. North = -z, south = +z, east = +x, west = -x, up = +y, down = -y;
  // the atlas is 2*(width+depth) wide and depth+height tall.
  UvRect rect;
  switch (face) {
    case Face::Top:
      rect = UvRect{u + depth, v, width, depth};
      break;
    case Face::Bottom:
      rect = UvRect{u + depth + width, v, width, depth};
      break;
    case Face::Left:
      rect = UvRect{u, v + depth, depth, height};
      break;
    case Face::Right:
      rect = UvRect{u + depth + width, v + depth, depth, height};
      break;
    case Face::Front:
      rect = UvRect{u + depth + width + depth, v + depth, width, height};
      break;
    case Face::Back:
      rect = UvRect{u + depth, v + depth, width, height};
      break;
  }
  return rect;
}

std::vector<Mesh> GeometryMeshBuilder::build(const BedrockGeometry &geometry) {
  std::vector<Mesh> meshes;
  if (geometry.models.empty()) return meshes;
  const GeometryModel &model = geometry.models.front();

  std::vector<const Bone *> byName;
  std::vector<std::string> names;
  byName.reserve(model.bones.size());
  names.reserve(model.bones.size());
  for (const Bone &bone : model.bones) {
    byName.push_back(&bone);
    names.push_back(bone.name);
  }

  for (const Bone &bone : model.bones) {
    if (bone.cubes.empty()) continue;
    const std::vector<Transform> chain = transformChain(bone, byName, names);

    Mesh mesh;
    mesh.bone = bone.name;

    for (const Cube &cube : bone.cubes) {
      const float inflate = cube.inflate;
      const float x0 = cube.origin.x - inflate;
      const float y0 = cube.origin.y - inflate;
      const float z0 = cube.origin.z - inflate;
      const float x1 = cube.origin.x + cube.size.x + inflate;
      const float y1 = cube.origin.y + cube.size.y + inflate;
      const float z1 = cube.origin.z + cube.size.z + inflate;

      Vec3f corner[8];
      for (int ix = 0; ix <= 1; ++ix) {
        for (int iy = 0; iy <= 1; ++iy) {
          for (int iz = 0; iz <= 1; ++iz) {
            Vec3f p{ix == 0 ? x0 : x1, iy == 0 ? y0 : y1, iz == 0 ? z0 : z1};
            if (cube.hasRotation) p = rotateAbout(p, cube.pivot, cube.rotation);
            p = applyChain(p, chain);
            corner[ix | (iy << 1) | (iz << 2)] = p;
          }
        }
      }

      for (int faceIndex = 0; faceIndex < kFaceCount; ++faceIndex) {
        const Face face = static_cast<Face>(faceIndex);

        // Per-face map wins; otherwise the box-UV unwrap, otherwise a 1x1 placeholder.
        UvRect rect;
        if (cube.perFace.present && cube.perFace.faces[faceIndex].valid()) {
          const UvRect raw = cube.perFace.faces[faceIndex];
          rect = UvRect{raw.u, raw.v, raw.w, raw.h};
        } else if (cube.hasBoxUv) {
          rect = boxUvForFace(face, cube.uv.x, cube.uv.y, cube.size.x, cube.size.y, cube.size.z);
        } else {
          rect = UvRect{0.0F, 0.0F, 1.0F, 1.0F};
        }

        // A negative UV size mirrors the sample. Fold the sign into the corner the rectangle is
        // measured from so the emitted texture coordinates are always increasing.
        const float u0 = rect.w < 0.0F ? rect.u + rect.w : rect.u;
        const float v0 = rect.h < 0.0F ? rect.v + rect.h : rect.v;
        const float u1 = u0 + std::fabs(rect.w);
        const float v1 = v0 + std::fabs(rect.h);

        const Vec3f normal = rotateVector(Vec3f{kFaceNormals[faceIndex][0], kFaceNormals[faceIndex][1],
                                                kFaceNormals[faceIndex][2]},
                                          chain.empty() ? Vec3f{} : chain.front().rotation);
        Vec3f shadedNormal = normal;
        for (std::size_t c = 1; c < chain.size(); ++c) {
          shadedNormal = rotateVector(shadedNormal, chain[c].rotation);
        }

        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        const int *indices = kFaceCorners[faceIndex];
        // Grid order [TL, TR, BL, BR] so the texture top maps to the model's +y edge on every face.
        const Vec2f uvs[4] = {Vec2f{u0, v0}, Vec2f{u1, v0}, Vec2f{u0, v1}, Vec2f{u1, v1}};
        for (int i = 0; i < 4; ++i) {
          const UvRect raw = cube.perFace.faces[faceIndex];
          Vec2f uv = uvs[i];
          if (cube.perFace.present && raw.w < 0.0F) uv.x = u0 + u1 - uv.x;
          if (cube.perFace.present && raw.h < 0.0F) uv.y = v0 + v1 - uv.y;
          mesh.vertices.push_back(Vertex{corner[indices[i]], shadedNormal, uv});
        }
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 1);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 0);
        mesh.indices.push_back(base + 2);
        mesh.indices.push_back(base + 3);
      }
    }

    if (!mesh.vertices.empty()) meshes.push_back(std::move(mesh));
  }
  return meshes;
}

} // namespace pl::cosmetics::geometry
