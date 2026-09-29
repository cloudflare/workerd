// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Checks that the compile caches embedded in the per-isolate bundle are actually consumed by the
// bootstrap. A rejected cache silently falls back to parsing from source in every isolate.
#include <workerd/io/observer.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

#include <atomic>

namespace workerd {
namespace {

struct CompileCacheCounts {
  std::atomic<int> found{0};
  std::atomic<int> rejected{0};
};

class CountingObserver final: public JsgIsolateObserver {
 public:
  explicit CountingObserver(CompileCacheCounts& counts): counts(counts) {}
  void onCompileCacheFound(v8::Isolate*) const override {
    counts.found.fetch_add(1, std::memory_order_relaxed);
  }
  void onCompileCacheRejected(v8::Isolate*) const override {
    counts.rejected.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  CompileCacheCounts& counts;
};

KJ_TEST("per-isolate bootstrap consumes its embedded compile caches") {
  CompileCacheCounts counts;

  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  // Load the whole bundle, including the streams scripts.
  flags.setTypeScriptImplementedStreams(true);

  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .autogates = kj::arr("per-isolate-javascript-bootstrap"_kj),
    .jsgIsolateObserver = kj::atomicRefcounted<CountingObserver>(counts),
  });
  // The bootstrap runs during context creation; this just ensures the worker is fully set up.
  fixture.runInIoContext([](const TestFixture::Environment&) {});

  KJ_EXPECT(counts.rejected.load() == 0,
      "V8 rejected per-isolate bootstrap compile caches; see the warnings logged above",
      counts.rejected.load());
  KJ_EXPECT(counts.found.load() > 0, "no bootstrap compile cache was consumed at all");
}

}  // namespace
}  // namespace workerd
