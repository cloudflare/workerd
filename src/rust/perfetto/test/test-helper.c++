#include "test-helper.h"

#include <workerd/util/use-perfetto-categories.h>

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/string.h>

#ifdef WORKERD_USE_PERFETTO
#include <stdio.h>
#include <unistd.h>

#include <kj/filesystem.h>
#endif

namespace workerd::rust::perfetto_test {

#ifdef WORKERD_USE_PERFETTO

namespace {

struct ActiveTrace {
  kj::Maybe<PerfettoSession> session;
  // A second descriptor for the session's (unlinked) trace file, to read it back.
  kj::OwnFd readFd;
};

kj::Maybe<ActiveTrace> activeTrace;

}  // namespace

bool perfetto_in_build() {
  return true;
}

void start_trace(::rust::Str categories) {
  KJ_REQUIRE(activeTrace == kj::none, "a trace is already active");
  FILE* file = tmpfile();
  KJ_REQUIRE(file != nullptr, "tmpfile() failed");
  KJ_DEFER(fclose(file));
  int sessionFd;
  KJ_SYSCALL(sessionFd = dup(fileno(file)));
  int readFd;
  KJ_SYSCALL(readFd = dup(sessionFd));
  activeTrace = ActiveTrace{
    .session = PerfettoSession(sessionFd, kj::heapString(categories.data(), categories.size())),
    .readFd = kj::OwnFd(readFd),
  };
}

void emit_cpp_events(size_t address) {
  auto ptr = reinterpret_cast<const void*>(address);
  { TRACE_EVENT("workerd", "cpp-scoped", PERFETTO_FLOW_FROM_POINTER(ptr)); }
  TRACE_EVENT_BEGIN("workerd", "cpp-begin", PERFETTO_TRACK_FROM_POINTER(ptr));
  TRACE_EVENT_END("workerd", PERFETTO_TRACK_FROM_POINTER(ptr));
  TRACE_COUNTER("workerd", "shared-counter", 42);
}

::rust::Vec<uint8_t> stop_trace() {
  auto& trace = KJ_REQUIRE_NONNULL(activeTrace, "no active trace");
  // Destroying the session flushes and stops it.
  trace.session = kj::none;

  ::rust::Vec<uint8_t> result;
  KJ_SYSCALL(lseek(trace.readFd.get(), 0, SEEK_SET));
  kj::byte buffer[4096];
  for (;;) {
    ssize_t n;
    KJ_SYSCALL(n = read(trace.readFd.get(), buffer, sizeof(buffer)));
    if (n == 0) break;
    for (auto b: kj::arrayPtr(buffer, n)) {
      result.push_back(b);
    }
  }
  activeTrace = kj::none;
  return result;
}

#else  // defined(WORKERD_USE_PERFETTO)

bool perfetto_in_build() {
  return false;
}

void start_trace(::rust::Str) {}

void emit_cpp_events(size_t) {}

::rust::Vec<uint8_t> stop_trace() {
  return {};
}

#endif  // defined(WORKERD_USE_PERFETTO)

}  // namespace workerd::rust::perfetto_test
