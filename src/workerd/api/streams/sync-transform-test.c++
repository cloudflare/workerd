// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sync-transform.h"

#include <workerd/tests/test-fixture.h>

namespace workerd::api {
namespace {

// The external memory the isolate has been told about. HeapStatistics::external_memory() counts
// only ArrayBuffer backing stores, so this reads the total by reporting no change.
int64_t externalMemory(v8::Isolate* isolate) {
  return isolate->AdjustAmountOfExternalAllocatedMemoryImpl(0);
}

// Copies its input to its output, holding `retained` bytes of configuration.
class ConfiguredStage final: public SyncTransformStage {
 public:
  explicit ConfiguredStage(size_t retained): retained(retained) {}

  void push(kj::ArrayPtr<const kj::byte> input, kj::Array<kj::byte>& output) override {
    output = kj::heapArray(input);
  }
  void end(kj::Array<kj::byte>& output) override {}
  size_t retainedBytes() const override {
    return retained;
  }

 private:
  size_t retained;
};

KJ_TEST("a sync transform charges its stage's configuration from creation until canceled") {
  constexpr int64_t CONFIGURATION = 4 * 1024 * 1024;
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto before = externalMemory(env.isolate);
    auto pair = newSyncTransformStreams(
        env.js, env.context, kj::heap<ConfiguredStage>(static_cast<size_t>(CONFIGURATION)), 1024);
    // Charged before anything is written.
    KJ_EXPECT(externalMemory(env.isolate) >= before + CONFIGURATION);
    auto canceled = pair.readable.cancel(env.js, kj::none);
    KJ_EXPECT(externalMemory(env.isolate) < before + CONFIGURATION);
    return env.context.awaitJs(env.js, kj::mv(canceled));
  });
}

KJ_TEST("fromRustBytes keeps a tight allocation and copies out a loose one") {
  ::rust::Vec<uint8_t> tight;
  tight.reserve(1000);
  for (size_t i = 0; i < 1000; ++i) tight.push_back(static_cast<uint8_t>(i));
  const uint8_t* tightData = tight.data();
  auto kept = fromRustBytes(kj::mv(tight));
  KJ_EXPECT(kept.begin() == tightData);
  KJ_EXPECT(kept.size() == 1000);

  // One byte left of a 64 KiB allocation, as after trimming a segment to a range: queuing it
  // would hold the whole allocation while charging one byte.
  ::rust::Vec<uint8_t> loose;
  loose.reserve(64 * 1024);
  loose.push_back(7);
  const uint8_t* looseData = loose.data();
  auto copied = fromRustBytes(kj::mv(loose));
  KJ_EXPECT(copied.begin() != looseData);
  KJ_EXPECT(copied.size() == 1 && copied[0] == 7);
}

}  // namespace
}  // namespace workerd::api
