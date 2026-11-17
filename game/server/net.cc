#include "net.h"

#include <unistd.h>

namespace mahjong::net {

void CloseFd(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

}  // namespace mahjong::net
