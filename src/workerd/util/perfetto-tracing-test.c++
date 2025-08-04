// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "perfetto-tracing.h"

#include <kj/debug.h>
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

void removeFile(const char* path) {
  unlink(path);
}

KJ_TEST("PerfettoSession basic functionality") {
  auto traceFile = getTempFileName("perfetto-test");

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

  removeFile(traceFile.cStr());
}

KJ_TEST("PerfettoSession with file descriptor") {
  auto traceFile = getTempFileName("perfetto-fd-test");

  int fd = open(traceFile.cStr(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  KJ_REQUIRE(fd >= 0);

  {
    PerfettoSession session(fd, "workerd");

    TRACE_EVENT("workerd", "fd_test_event");
    session.flush();
  }

  KJ_ASSERT(getFileSize(traceFile.cStr()) > 0);

  removeFile(traceFile.cStr());
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

  {
    PerfettoSession session(traceFile, "workerd,v8");

    TRACE_EVENT("workerd", "workerd_event");

    session.flush();
  }

  KJ_ASSERT(fileExists(traceFile.cStr()));
  removeFile(traceFile.cStr());
}

KJ_TEST("V8 Perfetto integration") {
  auto traceFile = getTempFileName("v8-perfetto-test");

  {
    PerfettoSession baselineSession(traceFile, "workerd");
    TRACE_EVENT("workerd", "baseline_event");
    baselineSession.flush();
  }
  removeFile(traceFile.cStr());

  {
    PerfettoSession session(traceFile, "v8,workerd");

    TRACE_EVENT("workerd", "v8_integration_test");

    KJ_ASSERT(TRACE_EVENT_CATEGORY_ENABLED("v8"));

    session.flush();
  }

  KJ_ASSERT(fileExists(traceFile.cStr()));
  size_t v8IntegratedSize = getFileSize(traceFile.cStr());
  KJ_ASSERT(v8IntegratedSize > 0);

  removeFile(traceFile.cStr());
}

KJ_TEST("Perfetto macros work when enabled") {
  auto traceFile = getTempFileName("perfetto-macros-test");

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

  removeFile(traceFile.cStr());
}

KJ_TEST("Perfetto configuration validation") {

  auto testFile = getTempFileName("config-validation");
  { PerfettoSession session(testFile, "workerd"); }
  removeFile(testFile.cStr());
}

#endif  // WORKERD_USE_PERFETTO

}  // namespace
}  // namespace workerd
