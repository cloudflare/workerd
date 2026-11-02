// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"
#include "perf-map.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

namespace workerd::jsg::test {
namespace {

// An anonymous temporary file, so the test runs wherever mkstemp() does (not just Linux).
kj::OwnFd newTempFile() {
  char path[] = "/tmp/perf-map-test-XXXXXX";
  auto fd = KJ_SYSCALL_FD(mkstemp(path));
  KJ_SYSCALL(unlink(path));
  return fd;
}

kj::OwnFd newAppendOnlyFile() {
  auto fd = newTempFile();
  KJ_SYSCALL(fcntl(fd, F_SETFL, O_APPEND));
  return fd;
}

kj::String readAll(int fd) {
  struct stat stats;
  KJ_SYSCALL(fstat(fd, &stats));
  auto result = kj::heapString(stats.st_size);
  ssize_t n;
  KJ_SYSCALL(n = pread(fd, result.begin(), result.size(), 0));
  KJ_ASSERT(n == stats.st_size);
  return result;
}

// The perf map has to be enabled before any isolate is created.
struct PerfMappedSystem {
  kj::OwnFd fd = newAppendOnlyFile();
  V8System system{{"--allow-natives-syntax"}, JitCodeEventTracking::YES};

  PerfMappedSystem() {
    system.enablePerfMap(KJ_SYSCALL_FD(dup(fd)));
  }
};
PerfMappedSystem perfMapped;

struct EvalContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(EvalContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(EvalIsolate, EvalContext);

KJ_TEST("PerfMapWriter writes one line per code block") {
  auto fd = newAppendOnlyFile();
  PerfMapWriter writer(KJ_SYSCALL_FD(dup(fd)));
  writer.add(reinterpret_cast<const void*>(0x7f00a0), 0x40, "JS:*f worker.js:1:2"_kj.asArray());
  // A newline in a name mustn't end the record early.
  writer.add(reinterpret_cast<const void*>(0x7f0100), 0x8, "JS:*a\nb"_kj.asArray());
  KJ_EXPECT(readAll(fd) == "7f00a0 40 JS:*f worker.js:1:2\n7f0100 8 JS:*a b\n");
}

KJ_TEST("PerfMapWriter requires O_APPEND") {
  auto fd = newTempFile();
  KJ_EXPECT_THROW_MESSAGE("O_APPEND", PerfMapWriter(kj::mv(fd)));
}

KJ_TEST("V8System records optimized code, but not bytecode, in the perf map") {
  Evaluator<EvalContext, EvalIsolate> e(perfMapped.system);
  e.expectEval(R"(
    function perfMapSquare(x) { return x * x; }
    %PrepareFunctionForOptimization(perfMapSquare);
    perfMapSquare(4);
    %OptimizeFunctionOnNextCall(perfMapSquare);
    perfMapSquare(5);
  )",
      "number", "25");

  auto map = readAll(perfMapped.fd);
  bool foundOptimized = false;
  kj::StringPtr rest = map;
  while (rest.size() > 0) {
    size_t end = rest.findFirst('\n').orDefault(rest.size());
    auto line = kj::str(rest.first(end));
    rest = rest.slice(kj::min(end + 1, rest.size()));
    if (!line.contains("perfMapSquare"_kj)) continue;
    // Bytecode never appears as a frame's PC, so interpreted functions have no entry.
    KJ_EXPECT(!line.contains("~perfMapSquare"_kj), line);
    if (line.contains("*perfMapSquare"_kj) || line.contains("*'perfMapSquare"_kj)) {
      foundOptimized = true;
    }
  }
  KJ_EXPECT(foundOptimized, map);
}

}  // namespace
}  // namespace workerd::jsg::test
