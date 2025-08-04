// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "perfetto-tracing.h"

#include <kj/debug.h>
#include <kj/io.h>
#include <kj/test.h>

#ifdef WORKERD_USE_PERFETTO
#include "use-perfetto-categories.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#endif  // WORKERD_USE_PERFETTO

#include <cstdlib>
#include <cstring>

namespace workerd {
namespace {

#ifdef WORKERD_USE_PERFETTO

kj::String getTempFileName(const char* prefix) {
  const char* tmpDir = getenv("TEST_TMPDIR");
  if (tmpDir == nullptr) tmpDir = "/tmp";
  return kj::str(tmpDir, "/", prefix, "-", rand(), ".pb");
}

bool fileExists(const char* path) {
  struct stat buffer;
  return (stat(path, &buffer) == 0);
}

size_t getFileSize(const char* path) {
  struct stat buffer;
  if (stat(path, &buffer) != 0) return 0;
  return buffer.st_size;
}

bool traceFileContainsEvent(const char* path, kj::StringPtr eventName) {
  kj::FdInputStream input(kj::OwnFd(open(path, O_RDONLY)));
  auto data = input.readAllBytes();

  if (data.size() < eventName.size()) return false;

  for (size_t i = 0; i <= data.size() - eventName.size(); ++i) {
    if (memcmp(data.begin() + i, eventName.begin(), eventName.size()) == 0) {
      return true;
    }
  }
  return false;
}

void removeFile(const char* path) {
  unlink(path);
}

KJ_TEST("PerfettoSession basic functionality") {
  auto traceFile = getTempFileName("perfetto-test");
  KJ_DEFER(removeFile(traceFile.cStr()));

  {
    PerfettoSession session(traceFile, "workerd");

    TRACE_EVENT("workerd", "test_event");
    TRACE_EVENT("workerd", "test_event_with_args", "test_arg", 42);

    TRACE_EVENT_BEGIN("workerd", "test_duration_event");
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    TRACE_EVENT_END("workerd");

    TRACE_EVENT_INSTANT("workerd", "test_instant_event");

    TRACE_COUNTER("workerd", "test_counter", 100);
    TRACE_COUNTER("workerd", "test_counter", 200);

    session.flush();
  }

  KJ_ASSERT(fileExists(traceFile.cStr()));
  KJ_ASSERT(getFileSize(traceFile.cStr()) > 0);

  KJ_ASSERT(traceFileContainsEvent(traceFile.cStr(), "test_event"));
  KJ_ASSERT(traceFileContainsEvent(traceFile.cStr(), "test_event_with_args"));
  KJ_ASSERT(traceFileContainsEvent(traceFile.cStr(), "test_duration_event"));
  KJ_ASSERT(traceFileContainsEvent(traceFile.cStr(), "test_instant_event"));
  KJ_ASSERT(traceFileContainsEvent(traceFile.cStr(), "test_counter"));
}

KJ_TEST("PerfettoSession with file descriptor") {
  auto traceFile = getTempFileName("perfetto-fd-test");
  KJ_DEFER(removeFile(traceFile.cStr()));

  int fd = open(traceFile.cStr(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  KJ_REQUIRE(fd >= 0);

  {
    PerfettoSession session(fd, "workerd");

    TRACE_EVENT("workerd", "fd_test_event");
    session.flush();
  }

  KJ_ASSERT(getFileSize(traceFile.cStr()) > 0);
  KJ_ASSERT(traceFileContainsEvent(traceFile.cStr(), "fd_test_event"));
}

KJ_TEST("PerfettoSession category parsing") {
  auto categories = PerfettoSession::parseCategories("cat1,cat2,cat3");
  KJ_ASSERT(categories.size() == 3);

  auto cat1 = kj::str(categories[0]);
  auto cat2 = kj::str(categories[1]);
  auto cat3 = kj::str(categories[2]);
  KJ_ASSERT(cat1 == "cat1");
  KJ_ASSERT(cat2 == "cat2");
  KJ_ASSERT(cat3 == "cat3");

  auto singleCat = PerfettoSession::parseCategories("single");
  KJ_ASSERT(singleCat.size() == 1);
  auto single = kj::str(singleCat[0]);
  KJ_ASSERT(single == "single");

  auto emptyCats = PerfettoSession::parseCategories("");
  KJ_ASSERT(emptyCats.size() == 0);
}

KJ_TEST("PerfettoSession multiple categories") {
  auto traceFile = getTempFileName("perfetto-multi-cat-test");
  KJ_DEFER(removeFile(traceFile.cStr()));

  {
    PerfettoSession session(traceFile, "workerd,v8");

    TRACE_EVENT("workerd", "workerd_event");

    session.flush();
  }

  KJ_ASSERT(fileExists(traceFile.cStr()));
  KJ_ASSERT(traceFileContainsEvent(traceFile.cStr(), "workerd_event"));
}

KJ_TEST("V8 Perfetto integration") {
  auto traceFile = getTempFileName("v8-perfetto-test");
  KJ_DEFER(removeFile(traceFile.cStr()));

  {
    PerfettoSession baselineSession(traceFile, "workerd");
    TRACE_EVENT("workerd", "baseline_event");
    baselineSession.flush();
  }

  {
    PerfettoSession session(traceFile, "v8,workerd");

    TRACE_EVENT("workerd", "v8_integration_test");

    KJ_ASSERT(TRACE_EVENT_CATEGORY_ENABLED("v8"));

    session.flush();
  }

  KJ_ASSERT(fileExists(traceFile.cStr()));
  size_t v8IntegratedSize = getFileSize(traceFile.cStr());
  KJ_ASSERT(v8IntegratedSize > 0);
  KJ_ASSERT(traceFileContainsEvent(traceFile.cStr(), "v8_integration_test"));
}

KJ_TEST("Perfetto macros work when enabled") {
  auto traceFile = getTempFileName("perfetto-macros-test");
  KJ_DEFER(removeFile(traceFile.cStr()));

  {
    PerfettoSession session(traceFile, "workerd");

    KJ_ASSERT(TRACE_EVENT_CATEGORY_ENABLED("workerd"));

    void* testPtr = reinterpret_cast<void*>(0x12345);
    auto flow = PERFETTO_FLOW_FROM_POINTER(testPtr);
    auto termFlow = PERFETTO_TERMINATING_FLOW_FROM_POINTER(testPtr);
    auto track = PERFETTO_TRACK_FROM_POINTER(testPtr);

    (void)flow;
    (void)termFlow;
    (void)track;
  }
}

KJ_TEST("Perfetto configuration validation") {
  auto testFile = getTempFileName("config-validation");
  KJ_DEFER(removeFile(testFile.cStr()));

  { PerfettoSession session(testFile, "workerd"); }
}

#endif  // WORKERD_USE_PERFETTO

}  // namespace
}  // namespace workerd
