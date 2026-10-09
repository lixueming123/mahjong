// Small POSIX socket helpers used by the gateway event loop.

#pragma once

#include <cstdint>
#include <string>

namespace mahjong::net {

// Creates a non-blocking listening socket bound to 127.0.0.1:port.
// Returns -1 and fills |*error| on failure.
int CreateListener(uint16_t port, std::string* error);

// Creates a non-blocking self-pipe; fds[0] is read, fds[1] is write.
bool CreateWakePipe(int fds[2], std::string* error);

void SetNonBlocking(int fd);
// Suppresses SIGPIPE on platforms providing SO_NOSIGPIPE (macOS/BSD).
void SetNoSigpipe(int fd);
void CloseFd(int fd);

}  // namespace mahjong::net
