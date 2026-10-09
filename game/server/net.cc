#include "net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mahjong::net {

void SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

void SetNoSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
  (void)fd;
#endif
}

void CloseFd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

int CreateListener(uint16_t port, std::string* error) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = std::string("socket() failed: ") + std::strerror(errno);
    return -1;
  }
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
    *error = "inet_pton(127.0.0.1) failed";
    CloseFd(fd);
    return -1;
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    *error = std::string("bind(127.0.0.1:") + std::to_string(port) + ") failed: " + std::strerror(errno);
    CloseFd(fd);
    return -1;
  }
  if (::listen(fd, 128) < 0) {
    *error = std::string("listen() failed: ") + std::strerror(errno);
    CloseFd(fd);
    return -1;
  }
  SetNonBlocking(fd);
  SetNoSigpipe(fd);
  return fd;
}

bool CreateWakePipe(int fds[2], std::string* error) {
  if (::pipe(fds) != 0) {
    *error = std::string("pipe() failed: ") + std::strerror(errno);
    return false;
  }
  SetNonBlocking(fds[0]);
  SetNonBlocking(fds[1]);
  return true;
}

}  // namespace mahjong::net
