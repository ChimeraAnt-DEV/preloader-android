/**
 * @file NativeCosmeticDataTest.cpp
 * @brief Host tests for the pure cosmetic frame/geometry parsing.
 *
 * The wire layout is hand-rolled and both sides must agree, so the exact bytes the device receiver
 * parses are asserted here: a wrong field order or a length-prefix bug is caught on the build host
 * rather than only as a garbled draw on a device.
 */

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "pl/cosmetics/NativeCosmeticData.hpp"

namespace {

// Little-endian writer mirroring the Java {@code CosmeticFrame} encoder.
struct Writer {
  std::vector<std::uint8_t> bytes;
  void u8(std::uint8_t v) { bytes.push_back(v); }
  void u32(std::uint32_t v) {
    bytes.push_back(v & 0xFF);
    bytes.push_back((v >> 8) & 0xFF);
    bytes.push_back((v >> 16) & 0xFF);
    bytes.push_back((v >> 24) & 0xFF);
  }
  void u16(std::uint16_t v) {
    bytes.push_back(v & 0xFF);
    bytes.push_back((v >> 8) & 0xFF);
  }
  void f32(float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    u32(bits);
  }
  void str16(const std::string &s) {
    u16(static_cast<std::uint16_t>(s.size()));
    for (char c : s) u8(static_cast<std::uint8_t>(c));
  }
};

constexpr std::uint32_t kMagic = 0x43484631;

void testFullFrameParses() {
  Writer w;
  w.u32(kMagic);
  w.u32(0x7u); // cape | accessory | pet
  w.u32(2);    // cape segments
  w.f32(-10.0F); // lean
  w.f32(3.0F);   // sway
  w.f32(-20.0F);
  w.f32(5.0F);
  w.u32(9); // accessory kind (CROWN-ish)
  w.f32(6.0F);  // head pitch
  w.f32(-14.0F); // head yaw
  w.u32(2); // pet bones
  w.str16("leg_a"); w.f32(0.0F); w.f32(0.0F); w.f32(30.0F);
  w.str16("head"); w.f32(0.0F); w.f32(15.0F); w.f32(0.0F);
  w.f32(20.0F); // pet offset x
  w.f32(1.0F);  // y
  w.f32(0.0F);  // z
  w.f32(0.4F);  // body bob
  w.f32(0.1F);  // crouch
  w.f32(0.0F);  // leanX

  std::uint32_t flags = 0;
  std::vector<float> leanSway;
  int kind = 0;
  float pitch = 0.0F, yaw = 0.0F;
  std::vector<std::pair<std::string, pl::cosmetics::Mat4>> bones;
  pl::cosmetics::Mat4 offset;
  const bool ok = pl::cosmetics::ParseCosmeticFrameData(
      std::span<const std::uint8_t>(w.bytes), flags, leanSway, kind, pitch, yaw, bones, offset);
  assert(ok);
  assert(flags == 0x7u);
  assert(leanSway.size() == 4);
  assert(std::fabs(leanSway[0] + 10.0F) < 1e-4F);
  assert(std::fabs(leanSway[3] - 5.0F) < 1e-4F);
  assert(kind == 9);
  assert(std::fabs(pitch - 6.0F) < 1e-4F);
  assert(std::fabs(yaw + 14.0F) < 1e-4F);
  assert(bones.size() == 2);
  assert(bones[0].first == "leg_a");
  assert(bones[1].first == "head");
  // petOffset = rotation(leanX) * translation(20, 1 + 0.4 - 0.1, 0); leanX=0 so translation only.
  // Column-major: translation sits in column 3 => at(row, col=3).
  assert(std::fabs(offset.at(0, 3) - 20.0F) < 1e-3F);
  assert(std::fabs(offset.at(1, 3) - 1.3F) < 1e-3F);
  assert(std::fabs(offset.at(2, 3)) < 1e-3F);
}

void testWrongMagicRejected() {
  Writer w;
  w.u32(0xDEADBEEFu);
  w.u32(0x7u);
  w.u32(0);
  std::uint32_t flags = 0;
  std::vector<float> leanSway;
  int kind = 0;
  float pitch = 0.0F, yaw = 0.0F;
  std::vector<std::pair<std::string, pl::cosmetics::Mat4>> bones;
  pl::cosmetics::Mat4 offset;
  const bool ok = pl::cosmetics::ParseCosmeticFrameData(
      std::span<const std::uint8_t>(w.bytes), flags, leanSway, kind, pitch, yaw, bones, offset);
  assert(!ok);
}

void testTruncatedFrameRejected() {
  std::vector<std::uint8_t> bytes = {static_cast<std::uint8_t>(kMagic >> 0),
                                     static_cast<std::uint8_t>(kMagic >> 8),
                                     static_cast<std::uint8_t>(kMagic >> 16),
                                     static_cast<std::uint8_t>(kMagic >> 24),
                                     0x01, 0x00, 0x00, 0x00}; // cape flag, but no rest
  std::uint32_t flags = 0;
  std::vector<float> leanSway;
  int kind = 0;
  float pitch = 0.0F, yaw = 0.0F;
  std::vector<std::pair<std::string, pl::cosmetics::Mat4>> bones;
  pl::cosmetics::Mat4 offset;
  const bool ok = pl::cosmetics::ParseCosmeticFrameData(
      std::span<const std::uint8_t>(bytes), flags, leanSway, kind, pitch, yaw, bones, offset);
  assert(!ok);
}

void testEmptyFrameRejected() {
  std::vector<std::uint8_t> bytes;
  std::uint32_t flags = 0;
  std::vector<float> leanSway;
  int kind = 0;
  float pitch = 0.0F, yaw = 0.0F;
  std::vector<std::pair<std::string, pl::cosmetics::Mat4>> bones;
  pl::cosmetics::Mat4 offset;
  const bool ok = pl::cosmetics::ParseCosmeticFrameData(
      std::span<const std::uint8_t>(bytes), flags, leanSway, kind, pitch, yaw, bones, offset);
  assert(!ok);
}

void testGeometryBlobParsing() {
  // Build a blob with two entries: one for a wrong id, one for the pet id.
  const std::string wrongId = "geometry.chimera_hat";
  const std::string petId = "geometry.chimera_pet";
  const std::string json =
      "{\"format_version\":\"1.8.0\",\"minecraft:geometry\":[{\"description\":{\"identifier\":"
      "\"geometry.chimera_pet\",\"texture_width\":64,\"texture_height\":64},\"bones\":["
      "{\"name\":\"pet\",\"pivot\":[0,0,0],\"cubes\":["
      "{\"origin\":[-2,0,-2],\"size\":[4,4,4],\"uv\":[0,0]}]}]}]}";

  std::vector<std::uint8_t> blob;
  auto append = [&](const std::string &id, const std::string &j) {
    const std::uint32_t idLen = static_cast<std::uint32_t>(id.size());
    const std::uint32_t jLen = static_cast<std::uint32_t>(j.size());
    blob.push_back(idLen & 0xFF);
    blob.push_back((idLen >> 8) & 0xFF);
    blob.push_back((idLen >> 16) & 0xFF);
    blob.push_back((idLen >> 24) & 0xFF);
    for (char c : id) blob.push_back(static_cast<std::uint8_t>(c));
    blob.push_back(jLen & 0xFF);
    blob.push_back((jLen >> 8) & 0xFF);
    blob.push_back((jLen >> 16) & 0xFF);
    blob.push_back((jLen >> 24) & 0xFF);
    for (char c : j) blob.push_back(static_cast<std::uint8_t>(c));
  };
  append(wrongId, json);
  append(petId, json);

  auto pieces = pl::cosmetics::ParseGeometryForPieceData(
      std::span<const std::uint8_t>(blob), "geometry.chimera_pet");
  assert(!pieces.empty());
  // One cube -> 6 faces x 4 corners = 24 vertices, 36 indices.
  assert(pieces.size() == 1);
  assert(pieces[0].mesh.vertices.size() == 24);
  assert(pieces[0].mesh.indices.size() == 36);
}

void testTruncatedGeometryBlobRejected() {
  std::vector<std::uint8_t> blob = {0x20, 0x00, 0x00, 0x00}; // declares 32-byte id but nothing
  auto pieces = pl::cosmetics::ParseGeometryForPieceData(
      std::span<const std::uint8_t>(blob), "geometry.chimera_pet");
  assert(pieces.empty());
}

} // namespace

int main() {
  testFullFrameParses();
  testWrongMagicRejected();
  testTruncatedFrameRejected();
  testEmptyFrameRejected();
  testGeometryBlobParsing();
  testTruncatedGeometryBlobRejected();
  return 0;
}