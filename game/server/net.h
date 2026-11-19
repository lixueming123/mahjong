// Descriptor helpers that outlived the hand-written event loop.
//
// asio owns the sockets, so one function is left: Connection still closes a descriptor it owns
// itself (owns_fd=true, which is the tests; the gateway passes false).

#pragma once

namespace mahjong::net {

void CloseFd(int fd);

}  // namespace mahjong::net
