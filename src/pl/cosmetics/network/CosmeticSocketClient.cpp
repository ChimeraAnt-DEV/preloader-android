/**
 * @file CosmeticSocketClient.cpp
 * @brief UDP multicast transport for cosmetic sync (Task 4).
 */

#include "pl/cosmetics/network/CosmeticSocketClient.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "pl/Logger.hpp"

namespace pl::cosmetics::network {

CosmeticSocketClient::~CosmeticSocketClient() { close(); }

bool CosmeticSocketClient::open(const std::string &group, std::uint16_t port) {
  close();
  mGroup = group;
  mPort = port;

  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    preloaderLogger.warn("CosmeticSync: socket() failed: {}", std::strerror(errno));
    return false;
  }

  int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
    preloaderLogger.warn("CosmeticSync: bind() failed: {}", std::strerror(errno));
    ::close(fd);
    return false;
  }

  ip_mreq membership{};
  membership.imr_multiaddr.s_addr = ::inet_addr(group.c_str());
  membership.imr_interface.s_addr = htonl(INADDR_ANY);
  if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) < 0) {
    preloaderLogger.warn("CosmeticSync: IP_ADD_MEMBERSHIP failed: {}", std::strerror(errno));
    ::close(fd);
    return false;
  }

  mFd = fd;
  preloaderLogger.info("CosmeticSync: multicast socket open on {}:{}", group, port);
  return true;
}

void CosmeticSocketClient::close() {
  if (mFd >= 0) {
    ip_mreq membership{};
    membership.imr_multiaddr.s_addr = ::inet_addr(mGroup.c_str());
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    ::setsockopt(mFd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &membership, sizeof(membership));
    ::close(mFd);
    mFd = -1;
  }
}

bool CosmeticSocketClient::send(const std::vector<std::uint8_t> &datagram) {
  if (mFd < 0 || datagram.empty()) return false;
  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = ::inet_addr(mGroup.c_str());
  destination.sin_port = htons(mPort);
  const ssize_t sent = ::sendto(mFd, datagram.data(), datagram.size(), 0,
                                reinterpret_cast<sockaddr *>(&destination), sizeof(destination));
  return sent == static_cast<ssize_t>(datagram.size());
}

bool CosmeticSocketClient::receive(std::vector<std::uint8_t> &out, int timeoutMs) {
  if (mFd < 0) return false;

  timeval timeout{};
  timeout.tv_sec = timeoutMs / 1000;
  timeout.tv_usec = (timeoutMs % 1000) * 1000;
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(mFd, &readSet);
  const int ready = ::select(mFd + 1, &readSet, nullptr, nullptr, &timeout);
  if (ready <= 0) return false;

  out.resize(kMaxPayload);
  sockaddr_in sender{};
  socklen_t senderLen = sizeof(sender);
  const ssize_t received = ::recvfrom(mFd, out.data(), out.size(), 0,
                                      reinterpret_cast<sockaddr *>(&sender), &senderLen);
  if (received <= 0) {
    out.clear();
    return false;
  }
  out.resize(static_cast<std::size_t>(received));
  return true;
}

} // namespace pl::cosmetics::network
