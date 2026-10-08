#pragma once

/**
 * @file BedrockGeometry.hpp
 * @brief Native Bedrock `.geo.json` geometry: the data shape plus the vertex-buffer builder.
 *
 * This is the native counterpart of the launcher's Java preview pipeline
 * (`core.cosmetics.geometry.BedrockGeometryParser` / `PreviewMeshModel`), moved into C++ so the
 * render seam can hand the GPU a mesh instead of only drawing a preview. It mirrors the same
 * conventions on purpose — the same face order, the same negative-UV mirror rule, the same
 * "world-space corners" resolution — so a model that looks right in the preview and one the native
 * pipeline attaches are the same geometry, not two interpretations that drift.
 *
 * <p><b>Format coverage.</b> Bedrock's format is loose and every looseness shows up in real
 * Blockbench exports: `format_version` is a string in 1.12 files and a number in 1.8 ones; a cube's
 * `size` may be one number or three; `uv` is either an origin+size pair (box UV) or a per-face
 * object; `inflate`, `rotation` and `pivot` are optional. The parser reads the tree by hand and
 * tolerates each variant, covering the two versions the launcher ships (1.12.0 and 1.21.0) and the
 * legacy 1.8.0 `geometry` key as a fallback. A missing optional field is left unset and the builder
 * falls back rather than throwing.
 *
 * <p><b>No resource packs, no Android.</b> Nothing here touches a `resource_packs/` directory or a
 * `PlayerSkinProvider`; the geometry is a plain JSON string from memory. No Android types are used,
 * so the whole parse and mesh build is unit-testable on the host (see `tests/cosmetics`).
 */

#include <cstdint>
#include <string>
#include <vector>

namespace pl::cosmetics::geometry {

/** @brief A 2-component float vector. */
struct Vec2f {
  float x = 0.0F;
  float y = 0.0F;
};

/** @brief A 3-component float vector. */
struct Vec3f {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

/**
 * @brief Cube face indices.
 *
 * The order matches the Java `PreviewMeshModel.FACE_CORNER_BITS` table and the per-face UV keys, so
 * the native and preview meshes agree on which rectangle belongs to which side.
 */
enum class Face : std::uint8_t {
  Top = 0,
  Bottom = 1,
  Left = 2,
  Right = 3,
  Front = 4,
  Back = 5,
};

/** @brief Number of faces on a cube. */
constexpr int kFaceCount = 6;

/**
 * @brief A source rectangle in atlas pixels.
 *
 * `w`/`h` may be negative: Blockbench emits a negative component whenever a face is mirrored, and
 * the mesh builder turns that sign into a texture-coordinate flip so the sample is not read
 * backwards.
 */
struct UvRect {
  float u = 0.0F;
  float v = 0.0F;
  float w = 0.0F;
  float h = 0.0F;

  /** @brief True when the rectangle is usable (non-zero area). */
  [[nodiscard]] bool valid() const { return w != 0.0F && h != 0.0F; }
};

/** @brief Per-face UV map, present when a cube carries an explicit map instead of box UV. */
struct UvPerFace {
  UvRect faces[kFaceCount];
  bool present = false;
};

/**
 * @brief A cube: an axis-aligned box in its bone's local space.
 *
 * `origin` is the minimum corner; `size` is `[width, height, depth]`. `uv`/`uvSize` are the box-UV
 * origin and unwrap size when the cube is not per-face mapped.
 */
struct Cube {
  Vec3f origin;
  Vec3f size;
  Vec3f rotation;
  Vec3f pivot;
  float inflate = 0.0F;
  bool hasRotation = false;
  UvPerFace perFace;
  Vec2f uv;      ///< box-UV origin, valid when `hasBoxUv`
  Vec2f uvSize;  ///< box-UV size, valid when `hasBoxUv`
  bool hasBoxUv = false;
};

/** @brief A named joint with an optional parent, a rotation pivot and its cubes. */
struct Bone {
  std::string name;
  std::string parent;
  Vec3f pivot;
  Vec3f rotation;
  bool hasPivot = false;
  bool hasRotation = false;
  std::vector<Cube> cubes;
};

/** @brief The geometry header: identifier and the atlas the UVs are written against. */
struct ModelDescription {
  std::string identifier;
  int textureWidth = 64;
  int textureHeight = 64;
};

/** @brief One geometry: a description plus a bone hierarchy. */
struct GeometryModel {
  ModelDescription description;
  std::vector<Bone> bones;
};

/** @brief A parsed `.geo.json` document: a format version and its models. */
struct BedrockGeometry {
  std::string formatVersion;
  std::vector<GeometryModel> models;
};

/** @brief A single interleaved vertex: model-space position, normal and atlas UV. */
struct Vertex {
  Vec3f position;
  Vec3f normal;
  Vec2f uv;
};

/**
 * @brief A resolved, renderable mesh for one bone.
 *
 * Corners are already transformed into model space (bone rotation, then each ancestor's), so a
 * caller can attach the mesh to a live bone matrix without re-walking the hierarchy per frame.
 */
struct Mesh {
  std::string bone;
  std::vector<Vertex> vertices;
  std::vector<std::uint32_t> indices;
};

/**
 * @brief Parses Bedrock `.geo.json` text into {@link BedrockGeometry}.
 *
 * Never throws on malformed input: a bad document returns `false` and leaves `out` untouched, which
 * the caller treats as "no authored mesh" and falls back. That is the same fail-closed contract the
 * preview parser uses, so one broken asset cannot take down the render path.
 */
class BedrockGeometryParser {
public:
  /**
   * @brief Parses `json` into `out`.
   * @param json  the `.geo.json` document
   * @param out   receives the parsed geometry on success
   * @param error optional; receives a human-readable reason on failure
   * @return true when at least one model with a bone was parsed
   */
  static bool parse(const std::string &json, BedrockGeometry &out, std::string *error = nullptr);

  /** @brief The format versions this parser recognises. */
  [[nodiscard]] static bool isSupportedFormat(const std::string &formatVersion);
};

/**
 * @brief A one-line human-readable summary of a parsed geometry.
 *
 * Pure (no logging dependency) so it is host-testable and the caller decides where it goes. The
 * native load path logs it to Logcat to confirm the multi-bone parse (hats/wings/pets) read from a
 * raw JSON string in memory, which is the Task 1 verification hook.
 */
std::string summarize(const BedrockGeometry &geometry);

/**
 * @brief Turns a parsed geometry into resolved meshes.
 *
 * Bedrock geometry is a bone hierarchy, not a flat box list: a cube's origin is stated in its bone's
 * local space, the bone rotates it about a pivot, and every ancestor's rotation applies on top.
 * Flattening that wrong is what makes an authored hat sit inside the head, so the builder resolves
 * the whole chain once per model change and emits plain model-space triangles.
 */
class GeometryMeshBuilder {
public:
  /**
   * @brief Builds one mesh per bone that carries cubes.
   * @param geometry a parsed model
   * @return the meshes, in bone declaration order
   */
  static std::vector<Mesh> build(const BedrockGeometry &geometry);

  /**
   * @brief Computes the box-UV rectangle for one face of a cube.
   *
   * Exposed so the layout rule (the standard Bedrock unwrap) is unit-testable independently of the
   * mesh assembly.
   */
  static UvRect boxUvForFace(Face face, float u, float v, float width, float height, float depth);
};

} // namespace pl::cosmetics::geometry
