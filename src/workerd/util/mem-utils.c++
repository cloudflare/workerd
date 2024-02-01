#include "mem-utils.h"

#include <kj/debug.h>
#include <kj/filesystem.h>

#if !_WIN32
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#endif

namespace workerd::util {

#if defined(__linux__)
kj::Maybe<size_t> tryGetResidentSetMemory() {
  int fd_;
  KJ_SYSCALL(fd_ = open("/proc/self/stat", O_RDONLY));
  kj::AutoCloseFd fd(fd_);
  auto text = kj::FdInputStream(kj::mv(fd)).readAllText();
  auto p = text.begin();
  for (kj::uint i = 0; i < 23; i++) {
    p = strchr(p, ' ');
    KJ_ASSERT(p != nullptr, "/proc/self/stat format not understood", text);
    ++p;
  }
  auto rss = strtoull(p, nullptr, 10);
  size_t result = rss * getpagesize();
  return result;
}
#elif defined(__APPLE__)
kj::Maybe<size_t> tryGetResidentSetMemory() {
  // TODO(soon): Implement etting the RSS for macOS
  return kj::none;
}
#elif defined(_WIN32)
kj::Maybe<size_t> tryGetResidentSetMemory() {
  // TODO(soon): Implement getting the RSS for Windows
  return kj::none;
}
#elif
kj::Maybe<size_t> tryGetResidentSetMemory() {
  // For all other platforms we simply return nothing
  return kj::none;
}
#endif

}  // namespace workerd::util
