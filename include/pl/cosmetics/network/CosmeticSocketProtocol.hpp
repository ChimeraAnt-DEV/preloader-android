#pragma once

/**
 * @file CosmeticSocketProtocol.hpp
 * @brief The native encoder/decoder for the cosmetic sync datagram (Task 4).
 *
 * **Byte-for-byte compatible with the Java `CosmeticSyncProtocol`.** The launcher and the native
 * module must interoperate on the same LAN: a native broadcast has to be understood by a Java
 * listener and vice versa, so this reproduces the exact layout — magic `CS`, version `1`, one type
 * byte, then five length-prefixed UTF-8 strings, each a 2-byte big-endian length followed by the
 * bytes (clipped to {@link kMaxString}). A peer identity (the "uuid") is `peerId`; the cosmetic id
 * set is `capeId`/`accessoryId`/`petId`. Only catalogue ids travel — the colour variant is part of
 * the id — so a datagram stays under 200 bytes and a new cosmetic is a catalogue entry, not a
 * protocol change.
 *
 * **A separate protocol from voice, on purpose.** Voice's format is pinned cross-language by golden
 * vectors against the Go relay; folding cosmetic fields in would break that pin. Each module ignores
 * the other's packets because the magic differs.
 *
 * Pure encoding/decoding with no socket and no Android types, so the round trip and the exact field
 * offsets are host-unit-testable — the same reason the geometry parser is.
 */

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pl/Export.hpp"

namespace pl::cosmetics::network {

/** @brief The datagram magic ('C', 'S'). */
inline constexpr std::uint8_t kMagic[2] = {'C', 'S'};
/** @brief The protocol version. */
inline constexpr std::uint8_t kVersion = 1;
/** @brief A full advertisement of the sender's equipped set. */
inline constexpr std::uint8_t kTypeAdvertise = 1;
/** @brief A prompt for peers to re-advertise immediately. */
inline constexpr std::uint8_t kTypeRequest = 2;
/** @brief Caps a string so a hostile sender cannot make us allocate without bound. */
inline constexpr int kMaxString = 64;
/** @brief Comfortably above the largest real datagram; sizes receive buffers. */
inline constexpr int kMaxPayload = 512;
/** @brief The id meaning "nothing equipped". */
inline constexpr const char *kNone = "none";

/** @brief A decoded datagram. Ids are never empty; an absent cosmetic reads as {@link kNone}. */
struct CosmeticAdvert {
  std::uint8_t type = kTypeAdvertise;
  std::string peerId;
  std::string name;
  std::string capeId = kNone;
  std::string accessoryId = kNone;
  std::string petId = kNone;

  /** @brief True when this is a request to re-advertise rather than an advertisement. */
  [[nodiscard]] bool isRequest() const { return type == kTypeRequest; }
};

/** @brief Pure encode/decode for the cosmetic sync datagram. */
class PL_EXPORT CosmeticSocketProtocol {
public:
  /** @brief Encodes a full advertisement. Empty/null ids become {@link kNone}. */
  static std::vector<std::uint8_t> encodeAdvert(std::string_view peerId, std::string_view name,
                                                std::string_view capeId,
                                                std::string_view accessoryId,
                                                std::string_view petId);

  /** @brief Encodes a request for peers to re-advertise immediately. */
  static std::vector<std::uint8_t> encodeRequest(std::string_view peerId, std::string_view name);

  /** @brief Decodes a datagram, or nullopt when it is not ours or is malformed. */
  static std::optional<CosmeticAdvert> decode(std::span<const std::uint8_t> data);

  /** @brief True when the first bytes are our magic and version. */
  static bool looksLikeCosmeticPacket(std::span<const std::uint8_t> data);
};

} // namespace pl::cosmetics::network
