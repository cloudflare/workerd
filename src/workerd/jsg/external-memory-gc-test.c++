// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct ExternalMemoryContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(ExternalMemoryContext) {}
};

JSG_DECLARE_ISOLATE_TYPE(ExternalMemoryIsolate, ExternalMemoryContext);

// Allocates 200 WebAssembly.Memory objects of 4 MiB each, touches every page, and drops each one
// before allocating the next. Very little of this is JS heap allocation, so only external memory
// pressure can start a full GC while it runs.
constexpr kj::StringPtr DROP_WASM_MEMORIES = R"(
  for (let i = 0; i < 200; i++) {
    const memory = new WebAssembly.Memory({ initial: 64 });
    new Uint8Array(memory.buffer).fill(1);
  }
  undefined;
)"_kj;

constexpr int64_t kMiB = 1024 * 1024;

// Records the external memory at the start of each full GC.
void recordPeak(v8::Isolate* isolate, v8::GCType, v8::GCCallbackFlags, void* data) {
  auto& peak = *static_cast<int64_t*>(data);
  peak = kj::max(peak, isolate->GetExternalMemory());
}

KJ_TEST("dropped WebAssembly.Memory is collected on external memory pressure") {
  Evaluator<ExternalMemoryContext, ExternalMemoryIsolate> e(v8System);
  int64_t peak = 0;
  e.run([&](auto& lock) {
    lock.v8Isolate->AddGCPrologueCallback(recordPeak, &peak, v8::GCType::kGCTypeMarkSweepCompact);
  });
  e.expectEval(DROP_WASM_MEMORIES, "undefined", "undefined");
  e.run([&](auto& lock) {
    peak = kj::max(peak, lock.v8Isolate->GetExternalMemory());
    lock.v8Isolate->RemoveGCPrologueCallback(recordPeak, &peak);
  });

  // Without a GC on external memory pressure, all 800 MiB that the loop allocates stays alive.
  KJ_EXPECT(peak < 200 * kMiB, peak);
}

}  // namespace
}  // namespace workerd::jsg::test
