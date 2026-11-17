// Descriptor helpers that outlived the hand-written event loop.
//
// asio owns the listening socket and every client socket now, so this is down to one function:
// Connection still closes a descriptor it owns itself when nothing else will (owns_fd=true, which
// is the tests; the gateway passes false because asio must not see a closed descriptor twice).

#pragma once

namespace mahjong::net {

void CloseFd(int fd);

}  // namespace mahjong::net
