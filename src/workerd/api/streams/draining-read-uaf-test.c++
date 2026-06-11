// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for a use-after-free in wrapDrainingRead.
//
// The bug: ReadableStreamJsController::drainingRead() wraps the inner promise from
// Consumer::drainingRead() with .then() callbacks that call endOperation() on the
// controller. These callbacks captured a raw `this` pointer to the controller with
// no strong reference keeping it alive. If the DrainingReader (which holds the only
// jsg::Ref<ReadableStream>) was destroyed while the promise was pending — e.g., due
// to coroutine cancellation in pumpToImpl — the controller was freed, and the .then()
// callbacks would access dangling memory.
//
// The fix adds `self = addRef()` captures to the wrapDrainingRead callbacks, keeping
// the stream (and controller) alive until the callbacks complete.
//
// This test reproduces the scenario:
//   1. Create a stream with an async pull (no immediate data).
//   2. Start a draining read → pending promise.
//   3. Enqueue data → resolves the inner promise, enqueueing microtasks.
//   4. Drop ALL external refs to the stream (reader + rs).
//   5. Run microtasks — the .then() callbacks fire.
//
// Without the fix, step 5 is a use-after-free on the controller's state member.
// With the fix, the self ref in the callbacks keeps the controller alive.
// ASAN catches the pre-fix version.

#include "readable.h"
#include "standard.h"

#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>

namespace workerd::api {
namespace {

void preamble(auto callback) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) { callback(env.js); });
}

jsg::JsValue toBytes(jsg::Lock& js, kj::StringPtr str) {
  return jsg::JsUint8Array::create(js, str.asBytes());
}

// Regression test: dropping the DrainingReader while a draining read promise is
// pending must not cause a use-after-free when the promise callbacks fire.
KJ_TEST("wrapDrainingRead ref prevents UAF when DrainingReader is dropped (value stream)") {
  preamble([](jsg::Lock& js) {
    // The pull callback saves a controller ref so we can enqueue data after
    // the draining read is pending. It deliberately does NOT enqueue data,
    // forcing drainingRead() into its async path.
    kj::Maybe<jsg::Ref<ReadableStreamDefaultController>> savedCtrl;

    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            if (savedCtrl == kj::none) {
              savedCtrl = c.addRef();
            }
            // Return resolved but do NOT enqueue data — this makes
            // drainingRead fall into the async path.
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Create a DrainingReader and start a read. The pull doesn't provide data,
    // so drainingRead() queues a ReadRequest and returns a pending promise.
    auto reader = KJ_ASSERT_NONNULL(DrainingReader::create(js, *rs));

    // Drop the stream. Since js.alloc() never created a CppGC shim (the stream
    // was only used from C++, never passed to JS), this is the last external
    // strong ref. Without the fix, maybeDeferDestruction (which runs immediately
    // under the lock) frees the ReadableStream and its ReadableStreamJsController.
    // With the fix, the self = addRef() captured in wrapDrainingRead's .then()
    // callbacks keeps the refcount > 0.
    // The reader still holds a jsg::Ref as long as it is active.
    { auto drop = kj::mv(rs); }

    bool readCompleted = false;
    auto promise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
      KJ_ASSERT(!result.done);
      KJ_ASSERT(result.chunks.size() == 1);
      KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "test");
      readCompleted = true;
    });

    // The pull should have been called, giving us a controller ref.
    auto& ctrl = KJ_ASSERT_NONNULL(savedCtrl);

    // Enqueue data. This resolves the pending ReadRequest inside the consumer,
    // which resolves the inner promise in the drainingRead chain. The .then()
    // microtasks are enqueued but NOT yet processed.
    ctrl->enqueue(js, toBytes(js, "test"));

    // Drop the saved controller ref — we no longer need it.
    savedCtrl = kj::none;

    // Drop the reader. ~DrainingReader releases the reader lock and drops its
    // jsg::Ref<ReadableStream>, which should be the last external ref to the
    // stream.
    { auto drop = kj::mv(reader); }

    // Process microtasks. The promise chain fires:
    //   inner .then() (Consumer level) → outer .then() (wrapDrainingRead) → our .then()
    //
    // Without fix: the outer .then() accesses this->state on the freed controller → UAF.
    // With fix: self ref keeps the controller alive through the callbacks.
    js.runMicrotasks();

    KJ_ASSERT(readCompleted, "draining read promise should have resolved with data");
  });
}

// Same test but for byte streams.
KJ_TEST("wrapDrainingRead ref prevents UAF when DrainingReader is dropped (byte stream)") {
  preamble([](jsg::Lock& js) {
    kj::Maybe<jsg::Ref<ReadableByteStreamController>> savedCtrl;

    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {}
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {
            if (savedCtrl == kj::none) {
              savedCtrl = c.addRef();
            }
            return js.resolvedPromise();
          }
        }
        KJ_UNREACHABLE;
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    auto reader = KJ_ASSERT_NONNULL(DrainingReader::create(js, *rs));

    bool readCompleted = false;
    auto promise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
      KJ_ASSERT(!result.done);
      KJ_ASSERT(result.chunks.size() == 1);
      KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "test");
      readCompleted = true;
    });

    auto& ctrl = KJ_ASSERT_NONNULL(savedCtrl);
    ctrl->enqueue(js, jsg::BufferSource(js, jsg::JsBufferSource(toBytes(js, "test"))));
    savedCtrl = kj::none;

    { auto drop = kj::mv(reader); }
    { auto drop = kj::mv(rs); }

    js.runMicrotasks();

    KJ_ASSERT(readCompleted, "draining read promise should have resolved with data");
  });
}

}  // namespace
}  // namespace workerd::api
