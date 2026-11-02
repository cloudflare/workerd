// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "perf-map.h"

#include <kj/debug.h>

#if !_WIN32
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace workerd::jsg {

#if _WIN32
PerfMapWriter::PerfMapWriter(kj::OwnFd fdParam): fd(kj::mv(fdParam)) {
  KJ_UNIMPLEMENTED("perf maps are not supported on Windows");
}

void PerfMapWriter::add(const void*, size_t, kj::ArrayPtr<const char>) const {
  KJ_UNIMPLEMENTED("perf maps are not supported on Windows");
}
#else
PerfMapWriter::PerfMapWriter(kj::OwnFd fdParam): fd(kj::mv(fdParam)) {
  int flags;
  KJ_SYSCALL(flags = fcntl(fd.get(), F_GETFL));
  KJ_REQUIRE(flags & O_APPEND, "perf map must be opened with O_APPEND");
}

void PerfMapWriter::add(const void* start, size_t size, kj::ArrayPtr<const char> name) const {
  if (failed.load(std::memory_order_relaxed)) return;

  // Function names are long only in pathological cases; truncating those is harmless.
  char line[1024];
  char* pos = kj::_::fillLimited(line, line + sizeof(line) - 1,
      kj::toCharSequence(kj::hex(reinterpret_cast<uintptr_t>(start))), kj::toCharSequence(' '),
      kj::toCharSequence(kj::hex(size)), kj::toCharSequence(' '));
  for (char c: name) {
    if (pos == line + sizeof(line) - 1) break;
    // A newline would end the record early, so names keep to one line.
    *pos++ = c == '\n' || c == '\r' ? ' ' : c;
  }
  *pos++ = '\n';

  ssize_t n;
  do {
    n = write(fd.get(), line, pos - line);
  } while (n < 0 && errno == EINTR);
  // A failed or short write (say, the disk filled up) may leave a partial line behind, so stop
  // rather than corrupt later lines.
  if (n != pos - line && !failed.exchange(true, std::memory_order_relaxed)) {
    KJ_LOG(ERROR, "failed to write perf map; no longer recording JIT code", n, errno);
  }
}
#endif  // _WIN32

}  // namespace workerd::jsg
