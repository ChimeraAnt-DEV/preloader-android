/**
 * @file NativeCosmeticData.cpp
 * @brief Pure parsing of the cosmetic frame + geometry blobs.
 *
 * Host-compilable (no Android, no GLES, no logging), so {@code tests/cosmetics/run_tests.sh} links
 * it with the geometry parser and the matrix module and asserts the exact wire layout the device
 * renderer consumes.
 */

#include "pl/cosmetics/NativeCosmeticData.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace pl::cosmetics {
namespace {

constexpr std::uint32_t kCosmeticFrameMagic = 0x43484631; // 'CHF1'
constexpr std::size_t kMaxBlobEntries = 16;
constexpr std::size_t kMaxBlobEntryBytes = 1u << 20; // 1 MiB per geometry entry

// Little-endian byte reader matching the Java {@code CosmeticFrame} encoder.
struct Reader {
  const std::uint8_t *p = nullptr;
  std::size_t len = 0;
  std::size_t off = 0;

  explicit Reader(std::span<const std::uint8_t> data) : p(data.data()), len(data.size()) {}

  [[nodiscard]] bool enough(std::size_t n) const { return off + n <= len; }

  bool u32(std::uint32_t &out) {
    if (!enough(4)) return false;
    out = static_cast<std::uint32_t>(p[off]) | (static_cast<std::uint32_t>(p[off + 1]) << 8) |
          (static_cast<std::uint32_t>(p[off + 2]) << 16) |
          (static_cast<std::uint32_t>(p[off + 3]) << 24);
    off += 4;
    return true;
  }

  bool u16(std::uint16_t &out) {
    if (!enough(2)) return false;
    out = static_cast<std::uint16_t>(p[off]) | (static_cast<std::uint16_t>(p[off + 1]) << 8);
    off += 2;
    return true;
  }

  bool f32(float &out) {
    std::uint32_t bits = 0;
    if (!u32(bits)) return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
  }

  bool str(std::size_t n, std::string &out) {
    if (!enough(n)) return false;
    out.assign(reinterpret_cast<const char *>(p + off), n);
    off += n;
    return true;
  }
};

} // namespace

bool ParseCosmeticFrameData(std::span<const std::uint8_t> frame, std::uint32_t &flags,
                            std::vector<float> &capeLeanSway, int &accessoryKind,
                            float &headPitchDeg, float &headYawDeg,
                            std::vector<std::pair<std::string, Mat4>> &petBones,
                            Mat4 &petOffset) {
  flags = 0;
  capeLeanSway.clear();
  accessoryKind = 0;
  headPitchDeg = 0.0F;
  headYawDeg = 0.0F;
  petBones.clear();
  petOffset = Mat4::identity();

  if (frame.empty() || frame.size() < 8) return false;
  Reader reader(frame);

  std::uint32_t magic = 0;
  if (!reader.u32(magic) || magic != kCosmeticFrameMagic) return false;
  if (!reader.u32(flags)) return false;

  std::uint32_t capeSegments = 0;
  if (!reader.u32(capeSegments) || capeSegments > 64) return false;
  capeLeanSway.reserve(capeSegments * 2);
  for (std::uint32_t i = 0; i < capeSegments * 2; ++i) {
    float value = 0.0F;
    if (!reader.f32(value) || !std::isfinite(value)) return false;
    capeLeanSway.push_back(value);
  }

  std::uint32_t kind = 0;
  if (!reader.u32(kind)) return false;
  accessoryKind = static_cast<int>(kind);
  if (!reader.f32(headPitchDeg) || !std::isfinite(headPitchDeg)) return false;
  if (!reader.f32(headYawDeg) || !std::isfinite(headYawDeg)) return false;

  std::uint32_t boneCount = 0;
  if (!reader.u32(boneCount) || boneCount > 128) return false;
  for (std::uint32_t i = 0; i < boneCount; ++i) {
    std::uint16_t nameLen = 0;
    if (!reader.u16(nameLen)) return false;
    std::string name;
    if (!reader.str(nameLen, name) || name.empty()) return false;
    float rx = 0.0F, ry = 0.0F, rz = 0.0F;
    if (!reader.f32(rx) || !std::isfinite(rx) || !reader.f32(ry) || !std::isfinite(ry) ||
        !reader.f32(rz) || !std::isfinite(rz)) {
      return false;
    }
    petBones.emplace_back(std::move(name), Mat4::rotationAboutPivot(Vec3{0, 0, 0}, Vec3{rx, ry, rz}));
  }

  float ox = 0.0F, oy = 0.0F, oz = 0.0F;
  float bob = 0.0F, crouch = 0.0F, leanX = 0.0F;
  if (!reader.f32(ox) || !reader.f32(oy) || !reader.f32(oz) || !reader.f32(bob) ||
      !reader.f32(crouch) || !reader.f32(leanX)) {
    return false;
  }
  if (!std::isfinite(ox) || !std::isfinite(oy) || !std::isfinite(oz) || !std::isfinite(bob) ||
      !std::isfinite(crouch) || !std::isfinite(leanX)) {
    return false;
  }
  petOffset = Mat4::multiply(Mat4::rotationAboutPivot(Vec3{0, 0, 0}, Vec3{leanX, 0, 0}),
                             Mat4::translation(Vec3{ox, oy + bob - crouch, oz}));
  return reader.off == reader.len;
}

std::vector<PieceMesh> ParseGeometryForPieceData(std::span<const std::uint8_t> blob,
                                                 std::string_view pieceId) {
  std::vector<PieceMesh> out;
  if (blob.empty() || pieceId.empty()) return out;

  Reader reader(blob);
  for (std::size_t entry = 0; entry < kMaxBlobEntries; ++entry) {
    if (reader.off >= reader.len) break;
    std::uint32_t idLen = 0;
    if (!reader.u32(idLen) || idLen > 256) return out;
    std::string id;
    if (!reader.str(idLen, id)) return out;

    std::uint32_t jsonLen = 0;
    if (!reader.u32(jsonLen) || jsonLen > kMaxBlobEntryBytes) return out;
    std::string json;
    if (!reader.str(jsonLen, json)) return out;

    if (id != pieceId) continue;
    pl::cosmetics::geometry::BedrockGeometry geometry;
    if (!pl::cosmetics::geometry::BedrockGeometryParser::parse(json, geometry)) return out;

    // Bone name -> pivot, for per-bone animation offsets.
    std::unordered_map<std::string, geometry::Vec3f> pivots;
    if (!geometry.models.empty()) {
      for (const auto &bone : geometry.models[0].bones) {
        if (bone.hasPivot) pivots[bone.name] = bone.pivot;
      }
    }
    auto meshes = pl::cosmetics::geometry::GeometryMeshBuilder::build(geometry);
    out.reserve(meshes.size());
    for (auto &mesh : meshes) {
      PieceMesh piece;
      piece.mesh = std::move(mesh);
      auto it = pivots.find(piece.mesh.bone);
      if (it != pivots.end()) {
        piece.pivot = Vec3{it->second.x, it->second.y, it->second.z};
        piece.hasPivot = true;
      }
      if (!piece.mesh.vertices.empty()) out.push_back(std::move(piece));
    }
    return out;
  }
  return out;
}

} // namespace pl::cosmetics