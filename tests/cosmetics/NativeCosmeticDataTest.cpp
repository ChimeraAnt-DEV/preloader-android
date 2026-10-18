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

// --- Cape/skin shape recognition -----------------------------------------------------------

// A minimal PNG header for a given IHDR size; the pixel data is irrelevant to the shape rule.
std::vector<std::uint8_t> pngHeader(std::uint32_t width, std::uint32_t height) {
  std::vector<std::uint8_t> p = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
  p.push_back(0x00); p.push_back(0x00); p.push_back(0x00); p.push_back(0x0d);  // IHDR length
  p.push_back('I'); p.push_back('H'); p.push_back('D'); p.push_back('R');
  auto be32 = [&p](std::uint32_t v) {
    p.push_back((v >> 24) & 0xFF);
    p.push_back((v >> 16) & 0xFF);
    p.push_back((v >> 8) & 0xFF);
    p.push_back(v & 0xFF);
  };
  be32(width);
  be32(height);
  return p;
}

void testPngDimensionsReadTheHeader() {
  auto png = pngHeader(64, 32);
  std::uint32_t w = 0, h = 0;
  assert(pl::cosmetics::PngDimensions(png.data(), png.size(), &w, &h));
  assert(w == 64 && h == 32);
  // A non-PNG is rejected rather than read as a size.
  std::vector<std::uint8_t> notPng = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                      17, 18, 19, 20, 21, 22, 23, 24};
  assert(!pl::cosmetics::PngDimensions(notPng.data(), notPng.size(), &w, &h));
  // A truncated header is rejected.
  assert(!pl::cosmetics::PngDimensions(png.data(), 12, &w, &h));
}

void testCapeShapeIsTheTwoToOnePanel() {
  assert(pl::cosmetics::IsCapeShapedSize(64, 32));
  assert(pl::cosmetics::IsCapeShapedSize(128, 64));
  // A square skin atlas is not a cape.
  assert(!pl::cosmetics::IsCapeShapedSize(64, 64));
  assert(!pl::cosmetics::IsCapeShapedSize(128, 128));
  // Other ratios are neither.
  assert(!pl::cosmetics::IsCapeShapedSize(64, 48));
  assert(!pl::cosmetics::IsCapeShapedSize(0, 0));
}

void testCapePanelFromPng() {
  auto png = pngHeader(64, 32);
  std::uint32_t w = 0, h = 0;
  assert(pl::cosmetics::CapePanelForPayload(0, png.data(), png.size(), &w, &h));
  assert(w == 64 && h == 32);
  // A square skin payload is not a cape, even through the same call.
  auto square = pngHeader(64, 64);
  assert(!pl::cosmetics::CapePanelForPayload(0, square.data(), square.size(), &w, &h));
}

void testCapePanelFromRgba8() {
  // A 64x32 RGBA8 panel is 64*32*4 bytes.
  std::vector<std::uint8_t> rgba(64 * 32 * 4, 0x80);
  std::uint32_t w = 0, h = 0;
  assert(pl::cosmetics::CapePanelForPayload(4, rgba.data(), rgba.size(), &w, &h));
  assert(w == 64 && h == 32);
  // A square RGBA8 buffer is not a cape.
  std::vector<std::uint8_t> square(64 * 64 * 4, 0x80);
  assert(!pl::cosmetics::CapePanelForPayload(4, square.data(), square.size(), &w, &h));
  // A wrong length is rejected.
  std::vector<std::uint8_t> odd(64 * 32 * 4 - 4, 0x80);
  assert(!pl::cosmetics::CapePanelForPayload(4, odd.data(), odd.size(), &w, &h));
}

void testCapePanelRejectsOtherFormatsAndNull() {
  auto png = pngHeader(64, 32);
  std::uint32_t w = 0, h = 0;
  assert(!pl::cosmetics::CapePanelForPayload(7, png.data(), png.size(), &w, &h));
  assert(!pl::cosmetics::CapePanelForPayload(0, nullptr, 100, &w, &h));
  assert(!pl::cosmetics::CapePanelForPayload(0, png.data(), 0, &w, &h));
}

} // namespace

int main() {
  testFullFrameParses();
  testWrongMagicRejected();
  testTruncatedFrameRejected();
  testEmptyFrameRejected();
  testGeometryBlobParsing();
  testTruncatedGeometryBlobRejected();
  testPngDimensionsReadTheHeader();
  testCapeShapeIsTheTwoToOnePanel();
  testCapePanelFromPng();
  testCapePanelFromRgba8();
  testCapePanelRejectsOtherFormatsAndNull();
  return 0;
}