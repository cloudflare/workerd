// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The C++ functions test_bridge.rs calls. They are in the bridge crate's link deps, not in
// io-context-test.c++, so that every binary linking the crate (its generated rust_test included)
// resolves them.

#include <workerd/io/io-context.h>
#include <workerd/io/io-context/test-bridge-state.h>
#include <workerd/io/io-context/test-bridge.h>

#include <bit>

namespace workerd::io_context::test {

kj::String recordedAsyncContext = kj::str("not recorded");
kj::Arc<jsg::AsyncContextFrame::StorageKey> TEST_KEY =
    kj::arc<jsg::AsyncContextFrame::StorageKey>();
kj::Maybe<uint64_t> awaitContextId;
kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> onAwaitCallback;

void record_await_context() {
  awaitContextId = IoContext::tryGetCurrentId().map(
      [](IoContext::Id id) { return std::bit_cast<uint64_t>(id); });
  KJ_IF_SOME(fulfiller, onAwaitCallback) {
    fulfiller->fulfill();
  }
}

void record_async_context(uintptr_t isolate) {
  auto& js = jsg::Lock::from(reinterpret_cast<v8::Isolate*>(isolate));
  recordedAsyncContext = kj::str("none");
  KJ_IF_SOME(frame, jsg::AsyncContextFrame::current(js)) {
    KJ_IF_SOME(value, frame.get(*TEST_KEY)) {
      recordedAsyncContext = jsg::JsValue(value.getHandle(js)).toString(js);
    }
  }
}

}  // namespace workerd::io_context::test
