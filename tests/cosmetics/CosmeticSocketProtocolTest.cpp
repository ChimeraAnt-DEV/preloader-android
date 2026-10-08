/**
 * @file CosmeticSocketProtocolTest.cpp
 * @brief Host unit tests for the native cosmetic sync datagram (Task 4).
 *
 * Pins the exact wire layout so the native and Java codecs cannot drift: magic, version, type, then
 * five 2-byte-big-endian-length-prefixed strings. A drift here is not a compile error — it is a
 * native peer the Java launcher silently ignores.
 */

#include "pl/cosmetics/network/CosmeticSocketProtocol.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace pl::cosmetics::network;

namespace {

int gChecks = 0;

void check(bool condition, const char *message) {
  ++gChecks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    assert(condition);
  }
}

void testExactLayout() {
  const std::vector<std::uint8_t> packet =
      CosmeticSocketProtocol::encodeAdvert("peer-1", "Chimera", "cape_red", "none", "pet_fox");
  // "CS" 01 01, then peer-1 (len 6), Chimera (len 7), cape_red (len 8), none (len 4), pet_fox (7).
  const std::vector<std::uint8_t> expected = {
      'C', 'S', 0x01, 0x01,
      0x00, 0x06, 'p', 'e', 'e', 'r', '-', '1',
      0x00, 0x07, 'C', 'h', 'i', 'm', 'e', 'r', 'a',
      0x00, 0x08, 'c', 'a', 'p', 'e', '_', 'r', 'e', 'd',
      0x00, 0x04, 'n', 'o', 'n', 'e',
      0x00, 0x07, 'p', 'e', 't', '_', 'f', 'o', 'x',
  };
  check(packet == expected, "advert layout matches the Java encoder");
}

void testRoundTrip() {
  const std::vector<std::uint8_t> packet =
      CosmeticSocketProtocol::encodeAdvert("uuid-1234", "Player", "cape_blue", "accessory_hat",
                                           "none");
  auto advert = CosmeticSocketProtocol::decode(packet);
  check(advert.has_value(), "advert decodes");
  check(advert->type == kTypeAdvertise, "advert type");
  check(advert->peerId == "uuid-1234", "peer id round-trips");
  check(advert->name == "Player", "name round-trips");
  check(advert->capeId == "cape_blue", "cape id round-trips");
  check(advert->accessoryId == "accessory_hat", "accessory id round-trips");
  check(advert->petId == "none", "none id round-trips");
  check(!advert->isRequest(), "advert is not a request");

  const std::vector<std::uint8_t> request =
      CosmeticSocketProtocol::encodeRequest("uuid-1234", "Player");
  auto decoded = CosmeticSocketProtocol::decode(request);
  check(decoded.has_value() && decoded->isRequest(), "request decodes");
  check(decoded->capeId == "none" && decoded->petId == "none", "request carries no cosmetics");
}

void testMissingIdsReadAsNone() {
  // A datagram with only the two mandatory strings (a future version that omits a field) must read
  // as "wearing nothing", not fail.
  std::vector<std::uint8_t> packet = {'C', 'S', 0x01, kTypeAdvertise,
                                      0x00, 0x02, 'p', '1',
                                      0x00, 0x00};
  auto advert = CosmeticSocketProtocol::decode(packet);
  check(advert.has_value(), "short advert still decodes");
  check(advert->capeId == "none" && advert->accessoryId == "none" && advert->petId == "none",
        "missing ids read as none");
}

void testStringClamp() {
  const std::string longName(200, 'x');
  const std::vector<std::uint8_t> packet =
      CosmeticSocketProtocol::encodeAdvert("p", "n", longName, "none", "none");
  auto advert = CosmeticSocketProtocol::decode(packet);
  check(advert.has_value(), "over-long id still decodes");
  check(advert->capeId.size() == static_cast<std::size_t>(kMaxString),
        "an over-long id is clamped, not rejected");
}

void testRejection() {
  auto decodeBytes = [](std::vector<std::uint8_t> bytes) {
    return CosmeticSocketProtocol::decode(std::span<const std::uint8_t>(bytes));
  };
  auto looksLike = [](std::vector<std::uint8_t> bytes) {
    return CosmeticSocketProtocol::looksLikeCosmeticPacket(std::span<const std::uint8_t>(bytes));
  };
  check(!decodeBytes({}).has_value(), "empty rejected");
  check(!decodeBytes({'X', 'S', 0x01, 0x01}).has_value(), "bad magic rejected");
  check(!decodeBytes({'C', 'S', 0x09, 0x01}).has_value(), "bad version rejected");
  check(!decodeBytes({'C', 'S', 0x01, 0x7f}).has_value(), "bad type rejected");
  check(looksLike({'C', 'S', 0x01}), "looksLikeCosmeticPacket accepts ours");
  check(!looksLike({'V', 'C', 0x04}), "looksLikeCosmeticPacket rejects voice");
}

} // namespace

int main() {
  testExactLayout();
  testRoundTrip();
  testMissingIdsReadAsNone();
  testStringClamp();
  testRejection();
  std::printf("CosmeticSocketProtocolTest: %d checks passed\n", gChecks);
  return 0;
}
