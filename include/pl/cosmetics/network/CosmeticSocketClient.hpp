#pragma once

/**
 * @file CosmeticSocketClient.hpp
 * @brief UDP transport for the cosmetic sync datagram (Task 4).
 *
 * Carries the {@link CosmeticSocketProtocol} datagram over the same LAN multicast group the launcher
 * uses (`239.255.42.100:47902`), so a native peer and a Java peer discover each other with no
 * server. The socket is managed explicitly (`open`/`close`) and a receive is non-blocking with a
 * timeout, so it can be polled from a game-frame tick without stalling the render thread.
 */

#include <cstdint>
#include <string>

#include "pl/cosmetics/network/CosmeticSocketProtocol.hpp"
#include "pl/Export.hpp"

namespace pl::cosmetics::network {

/** @brief The LAN multicast group and port the launcher's cosmetic sync uses. */
inline constexpr const char *kDefaultGroup = "239.255.42.100";
inline constexpr std::uint16_t kDefaultPort = 47902;

/** @brief A multicast UDP socket for cosmetic sync. */
class PL_EXPORT CosmeticSocketClient {
public:
  CosmeticSocketClient() = default;
  ~CosmeticSocketClient();

  CosmeticSocketClient(const CosmeticSocketClient &) = delete;
  CosmeticSocketClient &operator=(const CosmeticSocketClient &) = delete;

  /**
   * @brief Opens the socket and joins the multicast group.
   * @return true on success; false leaves the client closed (fail-closed — no packets, no crash)
   */
  bool open(const std::string &group = kDefaultGroup, std::uint16_t port = kDefaultPort);

  /** @brief Closes the socket. Safe to call when already closed. */
  void close();

  /** @brief True while the socket is open. */
  [[nodiscard]] bool isOpen() const { return mFd >= 0; }

  /** @brief Sends a datagram to the group. @return true when the full datagram left the socket. */
  bool send(const std::vector<std::uint8_t> &datagram);

  /**
   * @brief Receives one datagram, waiting up to @p timeoutMs.
   * @param out receives the bytes
   * @return true when a datagram arrived
   */
  bool receive(std::vector<std::uint8_t> &out, int timeoutMs);

private:
  int mFd = -1;
  std::string mGroup{kDefaultGroup};
  std::uint16_t mPort = kDefaultPort;
};

} // namespace pl::cosmetics::network
