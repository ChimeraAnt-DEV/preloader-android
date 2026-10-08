/**
 * @file CosmeticSocketProtocol.cpp
 * @brief Native cosmetic sync datagram codec (Task 4). Byte-compatible with Java CosmeticSyncProtocol.
 */

#include "pl/cosmetics/network/CosmeticSocketProtocol.hpp"

#include <cstring>

namespace pl::cosmetics::network {
namespace {

std::string normalizeId(std::string_view value) {
  return value.empty() ? std::string(kNone) : std::string(value);
}

/** @brief Appends a 2-byte big-endian length then the UTF-8 bytes, clipped to kMaxString. */
void writeString(std::vector<std::uint8_t> &out, std::string_view value) {
  std::size_t length = value.size();
  if (length > static_cast<std::size_t>(kMaxString)) length = kMaxString;
  out.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
  out.push_back(static_cast<std::uint8_t>(length & 0xFF));
  out.insert(out.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length));
}

/** @brief Reads a length-prefixed string; false on a truncated or over-long one. */
bool readString(std::span<const std::uint8_t> data, std::size_t &cursor, std::string &out) {
  if (cursor + 2 > data.size()) return false;
  const std::size_t length = (static_cast<std::size_t>(data[cursor]) << 8) | data[cursor + 1];
  cursor += 2;
  if (length > static_cast<std::size_t>(kMaxString)) return false;
  if (cursor + length > data.size()) return false;
  out.assign(reinterpret_cast<const char *>(data.data() + cursor), length);
  cursor += length;
  return true;
}

} // namespace

std::vector<std::uint8_t> CosmeticSocketProtocol::encodeAdvert(
    std::string_view peerId, std::string_view name, std::string_view capeId,
    std::string_view accessoryId, std::string_view petId) {
  std::vector<std::uint8_t> out;
  out.reserve(kMaxPayload);
  out.push_back(kMagic[0]);
  out.push_back(kMagic[1]);
  out.push_back(kVersion);
  out.push_back(kTypeAdvertise);
  writeString(out, peerId);
  writeString(out, name);
  writeString(out, normalizeId(capeId));
  writeString(out, normalizeId(accessoryId));
  writeString(out, normalizeId(petId));
  return out;
}

std::vector<std::uint8_t> CosmeticSocketProtocol::encodeRequest(std::string_view peerId,
                                                                std::string_view name) {
  std::vector<std::uint8_t> out;
  out.reserve(kMaxPayload);
  out.push_back(kMagic[0]);
  out.push_back(kMagic[1]);
  out.push_back(kVersion);
  out.push_back(kTypeRequest);
  writeString(out, peerId);
  writeString(out, name);
  writeString(out, kNone);
  writeString(out, kNone);
  writeString(out, kNone);
  return out;
}

bool CosmeticSocketProtocol::looksLikeCosmeticPacket(std::span<const std::uint8_t> data) {
  return data.size() >= 3 && data[0] == kMagic[0] && data[1] == kMagic[1] && data[2] == kVersion;
}

std::optional<CosmeticAdvert> CosmeticSocketProtocol::decode(std::span<const std::uint8_t> data) {
  if (data.size() < 4) return std::nullopt;
  if (data[0] != kMagic[0] || data[1] != kMagic[1]) return std::nullopt;
  if (data[2] != kVersion) return std::nullopt;
  const std::uint8_t type = data[3];
  if (type != kTypeAdvertise && type != kTypeRequest) return std::nullopt;

  std::size_t cursor = 4;
  CosmeticAdvert advert;
  advert.type = type;
  if (!readString(data, cursor, advert.peerId)) return std::nullopt;
  if (!readString(data, cursor, advert.name)) return std::nullopt;
  // Missing trailing ids read as "none", so a future version that omits a field degrades to
  // "wearing nothing" rather than failing to parse.
  advert.capeId = kNone;
  advert.accessoryId = kNone;
  advert.petId = kNone;
  readString(data, cursor, advert.capeId);
  readString(data, cursor, advert.accessoryId);
  readString(data, cursor, advert.petId);

  auto normalizeMissing = [](std::string &value) {
    if (value.empty()) value = kNone;
  };
  normalizeMissing(advert.capeId);
  normalizeMissing(advert.accessoryId);
  normalizeMissing(advert.petId);
  return advert;
}

} // namespace pl::cosmetics::network
