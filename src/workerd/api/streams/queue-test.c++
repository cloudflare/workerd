// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "queue.h"
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/jsg-test.h>

namespace workerd::api {
namespace {

jsg::V8System v8System;

struct QueueContext: public jsg::Object {
  JSG_RESOURCE_TYPE(QueueContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(QueueIsolate, QueueContext);

struct Preamble {
  QueueIsolate isolate;
  jsg::V8StackScope stackScope;
  QueueIsolate::Lock lock;
  v8::HandleScope scope;
  v8::Local<v8::Context> context;
  v8::Context::Scope contextScope;
  Preamble()
    : isolate(v8System),
      lock(isolate, stackScope),
      scope(lock.v8Isolate),
      context(lock.newContext<QueueContext>().getHandle(lock.v8Isolate)),
      contextScope(context) {}

  jsg::Lock& getJs() { return jsg::Lock::from(lock.v8Isolate); }
};

using ReadContinuation = jsg::Promise<ReadResult>(ReadResult&&);
using CloseContinuation = jsg::Promise<void>(ReadResult&&);
using ReadErrorContinuation = jsg::Promise<ReadResult>(jsg::Value&&);

kj::UnwindDetector unwindDetector;

template <typename Signature>
struct MustCall;
// Used to create a jsg::Promise continuation function that must be called
// at least once during the test. If the function is not called, an error
// will be thrown causing the test to fail.
// TODO(cleanup): Consider adding this to jsg-test.h

template <typename Signature>
struct MustNotCall;
// Used to create a jsg::Promise continuation function that must not be called
// during the test. If the function is called, an error will be thrown causing
// the test to fail.
// TODO(cleanup): Consider adding this to jsg-test.h

template <typename Ret, typename...Args>
struct MustCall<Ret(Args...)> {
  using Func = jsg::Function<Ret(Args...)>;
  Func fn;
  uint expected;
  kj::SourceLocation location;
  uint called = false;

  MustCall(Func fn, uint expected = 1, kj::SourceLocation location = kj::SourceLocation())
      : fn(kj::mv(fn)),
        expected(expected),
        location(location) {}

  ~MustCall() {
    if (!unwindDetector.isUnwinding()) {
      KJ_ASSERT(called == expected,
          kj::str("MustCall function was not called ", expected,
                  " times. [actual: ", called , "]"), location);
    }
  }

  Ret operator()(jsg::Lock& js, Args&&... args) {
    called++;
    return fn(js, kj::fwd<Args...>(args...));
  }
};

template <typename Ret, typename...Args>
struct MustNotCall<Ret(Args...)> {
  MustNotCall(kj::SourceLocation location = kj::SourceLocation()) : location(location) {}
  kj::SourceLocation location;
  Ret operator()(jsg::Lock&, Args...args) {
    KJ_FAIL_REQUIRE("MustNotCall function was called!", location);
  }
};

auto read(jsg::Lock& js, auto& consumer) {
  auto prp = js.newPromiseAndResolver<ReadResult>();
  consumer.read(js, ValueQueue::ReadRequest { .resolver = kj::mv(prp.resolver) });
  return kj::mv(prp.promise);
}

auto byobRead(jsg::Lock& js, auto& consumer, int size) {
  auto prp = js.newPromiseAndResolver<ReadResult>();
  consumer.read(js, ByteQueue::ReadRequest {
    .resolver = kj::mv(prp.resolver),
    .pullInto {
      .store = jsg::BackingStore::alloc(js, size),
      .type = ByteQueue::ReadRequest::Type::BYOB,
    },
  });
  return kj::mv(prp.promise);
};

auto getEntry(jsg::Lock& js, auto size) {
  return kj::refcounted<ValueQueue::Entry>(js.v8Ref(v8::True(js.v8Isolate).As<v8::Value>()), size);
}

#pragma region ValueQueue Tests

KJ_TEST("ValueQueue basics work") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ValueQueue queue(2);

  // At this point, there are no consumers, data does not get enqueued.
  KJ_ASSERT(queue.desiredSize() == 2);
  KJ_ASSERT(queue.size() == 0);

  queue.push(js, getEntry(js, 1));

  // Because there are no consumers, there is no change to backpressure.
  KJ_ASSERT(queue.desiredSize() == 2);
  KJ_ASSERT(queue.size() == 0);

  // Closing the queue causes the desiredSize to be zero.
  queue.close(js);

  try {
    queue.push(js, getEntry(js, 1));
    KJ_FAIL_ASSERT("The queue push after close should have failed.");
  } catch (kj::Exception& ex) {
    KJ_ASSERT(ex.getDescription().endsWith("The queue is closed or errored."));
  }

  KJ_ASSERT(queue.desiredSize() == 0);
  KJ_ASSERT(queue.size() == 0);
}

KJ_TEST("ValueQueue erroring works") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ValueQueue queue(2);

  queue.error(js, js.v8Ref(v8::Exception::Error(jsg::v8StrIntern(js.v8Isolate, "boom"_kj))));

  KJ_ASSERT(queue.desiredSize() == 0);

  try {
    queue.push(js, getEntry(js, 1));
    KJ_FAIL_ASSERT("The queue push after close should have failed.");
  } catch (kj::Exception& ex) {
    KJ_ASSERT(ex.getDescription().endsWith("The queue is closed or errored."));
  }
}

KJ_TEST("ValueQueue with single consumer") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ValueQueue queue(2);

  ValueQueue::Consumer consumer(queue);

  KJ_ASSERT(queue.desiredSize() == 2);

  queue.push(js, getEntry(js, 2));

  // The item was pushed into the consumer.
  KJ_ASSERT(consumer.size() == 2);

  // The queue size and desiredSize were updated accordingly.
  KJ_ASSERT(queue.size() == 2);
  KJ_ASSERT(queue.desiredSize() == 0);

  auto prp = js.newPromiseAndResolver<ReadResult>();
  consumer.read(js, ValueQueue::ReadRequest { .resolver = kj::mv(prp.resolver) });

  MustCall<ReadContinuation> readContinuation([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsTrue());

    KJ_ASSERT(consumer.size() == 0);
    KJ_ASSERT(queue.size() == 0);
    KJ_ASSERT(queue.desiredSize() == 2);

    return js.resolvedPromise(kj::mv(result));
  });

  prp.promise.then(js, readContinuation);

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ValueQueue with multiple consumers") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ValueQueue queue(2);

  ValueQueue::Consumer consumer1(queue);
  ValueQueue::Consumer consumer2(queue);

  KJ_ASSERT(queue.desiredSize() == 2);

  queue.push(js, getEntry(js, 2));

  // The item was pushed into the consumer.
  KJ_ASSERT(consumer1.size() == 2);
  KJ_ASSERT(consumer2.size() == 2);

  // The queue size and desiredSize were updated accordingly.
  KJ_ASSERT(queue.size() == 2);
  KJ_ASSERT(queue.desiredSize() == 0);

  MustCall<ReadContinuation> read1Continuation([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsTrue());

    KJ_ASSERT(consumer1.size() == 0);
    KJ_ASSERT(consumer2.size() == 2);

    // Backpressure was not relieved since the other consumer has yet to read.
    KJ_ASSERT(queue.size() == 2);
    KJ_ASSERT(queue.desiredSize() == 0);

    return read(js, consumer2);
  });

  MustCall<ReadContinuation> read2Continuation([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsTrue());

    KJ_ASSERT(consumer2.size() == 0);

    // Backpressure was relieved since both consumers have now read.
    KJ_ASSERT(queue.size() == 0);
    KJ_ASSERT(queue.desiredSize() == 2);

    return js.resolvedPromise(kj::mv(result));
  });

  MustCall<ReadContinuation> close1Continuation([&](jsg::Lock& js, auto&& result) {
    KJ_ASSERT(result.done);
    return read(js, consumer2);
  });

  MustCall<CloseContinuation> close2Continuation([&](jsg::Lock& js, auto&& result) {
    KJ_ASSERT(result.done);
    return js.resolvedPromise();
  });

  read(js, consumer1).then(js, read1Continuation)
                     .then(js, read2Continuation);

  js.v8Isolate->PerformMicrotaskCheckpoint();

  // Closing the queue causes both consumers to be closed...
  queue.close(js);

  // After close, the consumers will still be usable, but the queue itself
  // has shutdown and no longer reports backpressure.
  KJ_ASSERT(queue.desiredSize() == 0);
  KJ_ASSERT(queue.size() == 0);

  read(js, consumer1).then(js, close1Continuation)
                     .then(js, close2Continuation);

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ValueQueue consumer with multiple-reads") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ValueQueue queue(2);
  ValueQueue::Consumer consumer(queue);

  // The first read will produce a value.
  MustCall<ReadContinuation> read1Continuation([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsTrue());
    return js.resolvedPromise(kj::mv(result));
  });
  read(js, consumer).then(js, read1Continuation);

  // The second and third reads will both be done = true
  MustCall<CloseContinuation> closeContinuation([&](jsg::Lock& js, auto&& result) {
    KJ_ASSERT(result.done);
    return js.resolvedPromise();
  }, 2);

  read(js, consumer).then(js, closeContinuation);
  read(js, consumer).then(js, closeContinuation);

  queue.push(js, getEntry(js, 2));

  // Because there is a consumer reading when the push happens, no backpressure
  // is applied...
  KJ_ASSERT(queue.desiredSize() == 2);
  KJ_ASSERT(queue.size() == 0);

  queue.close(js);

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ValueQueue errors consumer with multiple-reads") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ValueQueue queue(2);
  ValueQueue::Consumer consumer(queue);

  MustCall<ReadErrorContinuation> errorContinuation([&](jsg::Lock& js, auto&& value) {
    KJ_ASSERT(value.getHandle(js)->IsNativeError());
    return js.rejectedPromise<ReadResult>(kj::mv(value));
  }, 3);
  MustNotCall<ReadContinuation> readContinuation;

  read(js, consumer).then(js, readContinuation, errorContinuation);
  read(js, consumer).then(js, readContinuation, errorContinuation);
  read(js, consumer).then(js, readContinuation, errorContinuation);

  queue.error(js, js.v8Ref(v8::Exception::Error(jsg::v8Str(js.v8Isolate, "boom"_kj))));

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ValueQueue with multiple consumers with pending reads") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ValueQueue queue(2);

  ValueQueue::Consumer consumer1(queue);
  ValueQueue::Consumer consumer2(queue);

  KJ_ASSERT(queue.desiredSize() == 2);

  MustCall<ReadContinuation> readContinuation([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsTrue());

    // Both reads were fulfilled immediately without buffering.
    KJ_ASSERT(consumer1.size() == 0);
    KJ_ASSERT(consumer2.size() == 0);

    // Backpressure is not signalled since both consumer reads have been
    // fulfilled.
    KJ_ASSERT(queue.size() == 0);
    KJ_ASSERT(queue.desiredSize() == 2);

    return js.resolvedPromise(kj::mv(result));
  }, 2);

  read(js, consumer1).then(js, readContinuation);
  read(js, consumer2).then(js, readContinuation);

  queue.push(js, getEntry(js, 2));

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

#pragma endregion ValueQueue Tests

#pragma region ByteQueue Tests

KJ_TEST("ByteQueue basics work") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ByteQueue queue(2);

  // At this point, there are no consumers, data does not get enqueued.
  KJ_ASSERT(queue.desiredSize() == 2);
  KJ_ASSERT(queue.size() == 0);

  auto entry = kj::refcounted<ByteQueue::Entry>(jsg::BackingStore::alloc(js, 4));

  queue.push(js, kj::mv(entry));

  // Because there are no consumers, there is no change to backpressure.
  KJ_ASSERT(queue.desiredSize() == 2);
  KJ_ASSERT(queue.size() == 0);

  // Closing the queue causes the desiredSize to be zero.
  queue.close(js);

  try {
    auto entry = kj::refcounted<ByteQueue::Entry>(jsg::BackingStore::alloc(js, 4));
    queue.push(js, kj::mv(entry));
    KJ_FAIL_ASSERT("The queue push after close should have failed.");
  } catch (kj::Exception& ex) {
    KJ_ASSERT(ex.getDescription().endsWith("The queue is closed or errored."));
  }

  KJ_ASSERT(queue.desiredSize() == 0);
  KJ_ASSERT(queue.size() == 0);
}

KJ_TEST("ByteQueue erroring works") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ByteQueue queue(2);

  queue.error(js, js.v8Ref(v8::Exception::Error(jsg::v8StrIntern(js.v8Isolate, "boom"_kj))));

  KJ_ASSERT(queue.desiredSize() == 0);

  try {
    auto entry = kj::refcounted<ByteQueue::Entry>(jsg::BackingStore::alloc(js, 4));
    queue.push(js, kj::mv(entry));
    KJ_FAIL_ASSERT("The queue push after close should have failed.");
  } catch (kj::Exception& ex) {
    KJ_ASSERT(ex.getDescription().endsWith("The queue is closed or errored."));
  }
}

KJ_TEST("ByteQueue with single consumer") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ByteQueue queue(2);

  ByteQueue::Consumer consumer(queue);

  KJ_ASSERT(queue.desiredSize() == 2);

  auto store = jsg::BackingStore::alloc(js, 4);
  memset(store.asArrayPtr().begin(), 'a', store.size());

  auto entry = kj::refcounted<ByteQueue::Entry>(kj::mv(store));
  queue.push(js, kj::mv(entry));

  // The item was pushed into the consumer.
  KJ_ASSERT(consumer.size() == 4);

  // The queue size and desiredSize were updated accordingly.
  KJ_ASSERT(queue.size() == 4);
  KJ_ASSERT(queue.desiredSize() == -2);

  auto prp = js.newPromiseAndResolver<ReadResult>();
  consumer.read(js, ByteQueue::ReadRequest {
    .resolver = kj::mv(prp.resolver),
    .pullInto {
      .store = jsg::BackingStore::alloc(js, 4),
    },
  });

  MustCall<ReadContinuation> readContinuation([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsArrayBufferView());
    jsg::BufferSource source(js, value.getHandle(js));
    KJ_ASSERT(source.size() == 4);
    KJ_ASSERT(source.asArrayPtr()[0] == 'a');
    KJ_ASSERT(source.asArrayPtr()[1] == 'a');
    KJ_ASSERT(source.asArrayPtr()[2] == 'a');
    KJ_ASSERT(source.asArrayPtr()[3] == 'a');

    KJ_ASSERT(consumer.size() == 0);
    KJ_ASSERT(queue.size() == 0);
    KJ_ASSERT(queue.desiredSize() == 2);

    return js.resolvedPromise(kj::mv(result));
  });

  prp.promise.then(js, readContinuation);

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ByteQueue with single byob consumer") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ByteQueue queue(2);

  ByteQueue::Consumer consumer(queue);

  auto prp = js.newPromiseAndResolver<ReadResult>();
  consumer.read(js, ByteQueue::ReadRequest {
    .resolver = kj::mv(prp.resolver),
    .pullInto {
      .store = jsg::BackingStore::alloc(js, 4),
      .type = ByteQueue::ReadRequest::Type::BYOB,
    },
  });

  MustCall<ReadContinuation> readContinuation([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsArrayBufferView());
    jsg::BufferSource source(js, value.getHandle(js));
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(source.size() == 3);
    KJ_ASSERT(ptr[0] == 'b');
    KJ_ASSERT(ptr[1] == 'b');
    KJ_ASSERT(ptr[2] == 'b');

    KJ_ASSERT(consumer.size() == 0);
    KJ_ASSERT(queue.size() == 0);
    KJ_ASSERT(queue.desiredSize() == 2);

    return js.resolvedPromise(kj::mv(result));
  });

  prp.promise.then(js, readContinuation);

  auto pendingByob = KJ_ASSERT_NONNULL(queue.nextPendingByobReadRequest());

  KJ_ASSERT(!pendingByob->isInvalidated());

  auto& req = pendingByob->getRequest();
  auto ptr = req.pullInto.store.asArrayPtr().begin();
  memset(ptr, 'b', 3);
  pendingByob->respond(js, 3);
  KJ_ASSERT(pendingByob->isInvalidated());

  // No backpressure is signaled.
  KJ_ASSERT(queue.desiredSize() == 2);
  KJ_ASSERT(queue.size() == 0);
  KJ_ASSERT(consumer.size() == 0);

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ByteQueue with byob consumer and default consumer") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ByteQueue queue(2);

  ByteQueue::Consumer consumer1(queue);
  ByteQueue::Consumer consumer2(queue);

  auto prp = js.newPromiseAndResolver<ReadResult>();
  consumer1.read(js, ByteQueue::ReadRequest {
    .resolver = kj::mv(prp.resolver),
    .pullInto {
      .store = jsg::BackingStore::alloc(js, 4),
      .type = ByteQueue::ReadRequest::Type::BYOB,
    },
  });

  MustCall<ReadContinuation> readContinuation([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsArrayBufferView());
    jsg::BufferSource source(js, value.getHandle(js));
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(source.size() == 3);
    KJ_ASSERT(ptr[0] == 'b');
    KJ_ASSERT(ptr[1] == 'b');
    KJ_ASSERT(ptr[2] == 'b');

    KJ_ASSERT(consumer1.size() == 0);
    KJ_ASSERT(consumer2.size() == 3);
    KJ_ASSERT(queue.size() == 3);
    KJ_ASSERT(queue.desiredSize() == -1);

    return js.resolvedPromise(kj::mv(result));
  });

  prp.promise.then(js, readContinuation);

  auto pendingByob = KJ_ASSERT_NONNULL(queue.nextPendingByobReadRequest());

  KJ_ASSERT(!pendingByob->isInvalidated());

  auto& req = pendingByob->getRequest();
  auto ptr = req.pullInto.store.asArrayPtr().begin();
  memset(ptr, 'b', 3);
  pendingByob->respond(js, 3);
  KJ_ASSERT(pendingByob->isInvalidated());

  // Backpressure is signaled because the other consumer hasn't been read from.
  KJ_ASSERT(queue.desiredSize() == -1);
  KJ_ASSERT(queue.size() == 3);
  KJ_ASSERT(consumer1.size() == 0);
  KJ_ASSERT(consumer2.size() == 3);

  js.v8Isolate->PerformMicrotaskCheckpoint();

  MustCall<ReadContinuation> read2Continuation([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsArrayBufferView());
    jsg::BufferSource source(js, value.getHandle(js));
    auto ptr = source.asArrayPtr();
    // The second consumer receives exactly the same data.
    KJ_ASSERT(source.size() == 3);
    KJ_ASSERT(ptr[0] == 'b');
    KJ_ASSERT(ptr[1] == 'b');
    KJ_ASSERT(ptr[2] == 'b');

    // The backpressure in the queue has been resolved.
    KJ_ASSERT(queue.size() == 0);
    KJ_ASSERT(queue.desiredSize() == 2);

    return js.resolvedPromise(kj::mv(result));
  });

  auto prp2 = js.newPromiseAndResolver<ReadResult>();
  consumer2.read(js, ByteQueue::ReadRequest {
    .resolver = kj::mv(prp2.resolver),
    .pullInto {
      .store = jsg::BackingStore::alloc(js, 4),
      .type = ByteQueue::ReadRequest::Type::DEFAULT,
    },
  });
  prp2.promise.then(js, read2Continuation);

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ByteQueue with multiple byob consumers") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ByteQueue queue(2);

  ByteQueue::Consumer consumer1(queue);
  ByteQueue::Consumer consumer2(queue);

  MustCall<ReadContinuation> readContinuation([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsArrayBufferView());
    jsg::BufferSource source(js, value.getHandle(js));
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(source.size() == 3);
    KJ_ASSERT(ptr[0] == 'b');
    KJ_ASSERT(ptr[1] == 'b');
    KJ_ASSERT(ptr[2] == 'b');

    KJ_ASSERT(consumer1.size() == 0);
    KJ_ASSERT(consumer2.size() == 0);
    KJ_ASSERT(queue.size() == 0);
    KJ_ASSERT(queue.desiredSize() == 2);

    return js.resolvedPromise(kj::mv(result));
  }, 2);

  // Both reads will receive the data despite there being only a single
  // byob read responded to.
  byobRead(js, consumer1, 4).then(js, readContinuation);
  byobRead(js, consumer2, 4).then(js, readContinuation);

  auto pendingByob = KJ_ASSERT_NONNULL(queue.nextPendingByobReadRequest());
  auto nextPending = KJ_ASSERT_NONNULL(queue.nextPendingByobReadRequest());

  KJ_ASSERT(!pendingByob->isInvalidated());

  auto& req = pendingByob->getRequest();
  auto ptr = req.pullInto.store.asArrayPtr().begin();
  memset(ptr, 'b', 3);
  pendingByob->respond(js, 3);
  KJ_ASSERT(pendingByob->isInvalidated());

  // No backpressure is signaled because both reads were fulfilled.
  KJ_ASSERT(queue.desiredSize() == 2);
  KJ_ASSERT(queue.size() == 0);
  KJ_ASSERT(consumer1.size() == 0);
  KJ_ASSERT(consumer2.size() == 0);

  // The next pendingByobReadRequest was invalidated.
  KJ_ASSERT(nextPending->isInvalidated());
  KJ_ASSERT(queue.nextPendingByobReadRequest() == nullptr);

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ByteQueue with multiple byob consumers") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ByteQueue queue(2);

  ByteQueue::Consumer consumer1(queue);
  ByteQueue::Consumer consumer2(queue);

  MustCall<ReadContinuation> readContinuation([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsArrayBufferView());
    jsg::BufferSource source(js, value.getHandle(js));
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(source.size() == 3);
    KJ_ASSERT(ptr[0] == 'b');
    KJ_ASSERT(ptr[1] == 'b');
    KJ_ASSERT(ptr[2] == 'b');

    KJ_ASSERT(consumer1.size() == 0);
    KJ_ASSERT(consumer2.size() == 0);
    KJ_ASSERT(queue.size() == 0);
    KJ_ASSERT(queue.desiredSize() == 2);

    return js.resolvedPromise(kj::mv(result));
  }, 2);

  // Both reads will receive the data despite there being only a single
  // byob read responded to.
  byobRead(js, consumer1, 4).then(js, readContinuation);
  byobRead(js, consumer2, 4).then(js, readContinuation);

  auto pendingByob = KJ_ASSERT_NONNULL(queue.nextPendingByobReadRequest());
  auto nextPending = KJ_ASSERT_NONNULL(queue.nextPendingByobReadRequest());

  KJ_ASSERT(!pendingByob->isInvalidated());

  auto& req = pendingByob->getRequest();
  auto ptr = req.pullInto.store.asArrayPtr().begin();
  memset(ptr, 'b', 3);
  pendingByob->respond(js, 3);
  KJ_ASSERT(pendingByob->isInvalidated());

  // No backpressure is signaled because both reads were fulfilled.
  KJ_ASSERT(queue.desiredSize() == 2);
  KJ_ASSERT(queue.size() == 0);
  KJ_ASSERT(consumer1.size() == 0);
  KJ_ASSERT(consumer2.size() == 0);

  // The next pendingByobReadRequest was invalidated.
  KJ_ASSERT(nextPending->isInvalidated());
  KJ_ASSERT(queue.nextPendingByobReadRequest() == nullptr);

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ByteQueue with multiple byob consumers (multi-reads)") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ByteQueue queue(2);

  ByteQueue::Consumer consumer1(queue);
  ByteQueue::Consumer consumer2(queue);

  MustCall<ReadContinuation> readConsumer1([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsArrayBufferView());
    jsg::BufferSource source(js, value.getHandle(js));
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(source.size() == 3);
    KJ_ASSERT(ptr[0] == 'a');
    KJ_ASSERT(ptr[1] == 'a');
    KJ_ASSERT(ptr[2] == 'a');

    return js.resolvedPromise(kj::mv(result));
  });

  MustCall<ReadContinuation> readConsumer2([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsArrayBufferView());
    jsg::BufferSource source(js, value.getHandle(js));
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(source.size() == 3);
    KJ_ASSERT(ptr[0] == 'a');
    KJ_ASSERT(ptr[1] == 'a');
    KJ_ASSERT(ptr[2] == 'a');

    return byobRead(js, consumer2, 4);
  });

  MustCall<ReadContinuation> secondReadBothConsumers([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsArrayBufferView());
    jsg::BufferSource source(js, value.getHandle(js));
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(source.size() == 2);
    KJ_ASSERT(ptr[0] == 'b');
    KJ_ASSERT(ptr[1] == 'b');

    return js.resolvedPromise(kj::mv(result));
  }, 2);

  // All reads will be fulfilled correctly even tho there are only two byob
  // reads processed.
  byobRead(js, consumer1, 4).then(js, readConsumer1);
  byobRead(js, consumer1, 4).then(js, secondReadBothConsumers);
  byobRead(js, consumer2, 4).then(js, readConsumer2)
                 .then(js, secondReadBothConsumers);

  // Although there are four distinct reads happening,
  // there should only be two actual BYOB requests
  // processed by the queue, which will fulfill all four
  // reads.
  MustCall<void(ByteQueue::ByobRequest&)> respond([&](jsg::Lock&, auto& pending) {
    static uint counter = 0;
    auto& req = pending.getRequest();
    auto ptr = req.pullInto.store.asArrayPtr().begin();
    auto num = 3 - counter;
    memset(ptr, 'a' + counter++, num);
    pending.respond(js, num);
    KJ_ASSERT(pending.isInvalidated());
  }, 2);

  kj::Maybe<kj::Own<ByteQueue::ByobRequest>> pendingByob;
  while ((pendingByob = queue.nextPendingByobReadRequest()) != nullptr) {
    auto& pending = KJ_ASSERT_NONNULL(pendingByob);
    if (pending->isInvalidated()) {
      continue;
    }
    respond(js, *pending);
  }

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ByteQueue with multiple byob consumers (multi-reads, 2)") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ByteQueue queue(2);

  ByteQueue::Consumer consumer1(queue);
  ByteQueue::Consumer consumer2(queue);

  MustCall<ReadContinuation> readConsumer1([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsArrayBufferView());
    jsg::BufferSource source(js, value.getHandle(js));
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(source.size() == 3);
    KJ_ASSERT(ptr[0] == 'a');
    KJ_ASSERT(ptr[1] == 'a');
    KJ_ASSERT(ptr[2] == 'a');
    return js.resolvedPromise(kj::mv(result));
  });

  MustCall<ReadContinuation> readConsumer2([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsArrayBufferView());
    jsg::BufferSource source(js, value.getHandle(js));
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(source.size() == 3);
    KJ_ASSERT(ptr[0] == 'a');
    KJ_ASSERT(ptr[1] == 'a');
    KJ_ASSERT(ptr[2] == 'a');

    return byobRead(js, consumer2, 4);
  });

  MustCall<ReadContinuation> secondReadBothConsumers([&](jsg::Lock& js, auto&& result) -> auto {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    KJ_ASSERT(value.getHandle(js)->IsArrayBufferView());
    jsg::BufferSource source(js, value.getHandle(js));
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(source.size() == 2);
    KJ_ASSERT(ptr[0] == 'b');
    KJ_ASSERT(ptr[1] == 'b');

    return js.resolvedPromise(kj::mv(result));
  }, 2);

  // All reads will be fulfilled correctly even tho there are only two BYOB reads
  // responded to.
  byobRead(js, consumer2, 4).then(js, readConsumer2)
                            .then(js, secondReadBothConsumers);
  byobRead(js, consumer1, 4).then(js, readConsumer1);
  byobRead(js, consumer1, 4).then(js, secondReadBothConsumers);

  // Although there are four distinct reads happening,
  // there should only be two actual BYOB requests
  // processed by the queue, which will fulfill all four
  // reads.
  MustCall<void(ByteQueue::ByobRequest&)> respond([&](jsg::Lock&, auto& pending) {
    static uint counter = 0;
    auto& req = pending.getRequest();
    auto ptr = req.pullInto.store.asArrayPtr().begin();
    auto num = 3 - counter;
    memset(ptr, 'a' + counter++, num);
    pending.respond(js, num);
    KJ_ASSERT(pending.isInvalidated());
  }, 2);

  kj::Maybe<kj::Own<ByteQueue::ByobRequest>> pendingByob;
  while ((pendingByob = queue.nextPendingByobReadRequest()) != nullptr) {
    auto& pending = KJ_ASSERT_NONNULL(pendingByob);
    if (pending->isInvalidated()) {
      continue;
    }
    respond(js, *pending);
  }

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ByteQueue with default consumer with atLeast") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ByteQueue queue(2);

  ByteQueue::Consumer consumer(queue);

  const auto read = [&](jsg::Lock& js, uint atLeast) {
    auto prp = js.newPromiseAndResolver<ReadResult>();
    consumer.read(js, ByteQueue::ReadRequest {
      .resolver = kj::mv(prp.resolver),
      .pullInto = {
        .store = jsg::BackingStore::alloc(js, 5),
        .atLeast = atLeast,
      },
    });
    return kj::mv(prp.promise);
  };

  const auto push = [&](auto store) {
    try {
      queue.push(js, kj::refcounted<ByteQueue::Entry>(kj::mv(store)));
    } catch (kj::Exception& ex) {
      KJ_DBG(ex.getDescription());
    }
  };

  MustCall<ReadContinuation> readContinuation([&](jsg::Lock& js, auto&& result) {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    auto view = value.getHandle(js);
    KJ_ASSERT(view->IsArrayBufferView());
    jsg::BufferSource source(js, view);
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(ptr[0] == 1);
    KJ_ASSERT(ptr[1] == 2);
    KJ_ASSERT(ptr[2] == 3);
    KJ_ASSERT(ptr[3] == 4);
    KJ_ASSERT(ptr[4] == 5);
    KJ_ASSERT(source.size(), 5);
    KJ_ASSERT(consumer.size(), 1);
    return read(js, 1);
  });

  MustCall<ReadContinuation> read2Continuation([&](jsg::Lock& js, auto&& result) {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    auto view = value.getHandle(js);
    KJ_ASSERT(view->IsArrayBufferView());
    jsg::BufferSource source(js, view);
    KJ_ASSERT(source.asArrayPtr()[0], 6);
    KJ_ASSERT(source.size() == 1);
    return js.resolvedPromise(kj::mv(result));
  });

  read(js, 5).then(js, readContinuation)
             .then(js, read2Continuation);

  auto store1 = jsg::BackingStore::alloc(js, 2);
  store1.asArrayPtr()[0] = 1;
  store1.asArrayPtr()[1] = 2;
  push(kj::mv(store1));

  KJ_ASSERT(queue.desiredSize() == 0);

  auto store2 = jsg::BackingStore::alloc(js, 2);
  store2.asArrayPtr()[0] = 3;
  store2.asArrayPtr()[1] = 4;
  push(kj::mv(store2));

  // Backpressure should be accumulating because the read has not yet fullilled.
  KJ_ASSERT(queue.desiredSize() == -2);

  auto store3 = jsg::BackingStore::alloc(js, 2);
  store3.asArrayPtr()[0] = 5;
  store3.asArrayPtr()[1] = 6;
  push(kj::mv(store3));

  // Some backpressure should be released because pushing the final minimum
  // amount into the queue should have caused the read to be fulfilled.
  KJ_ASSERT(queue.desiredSize() == 1);

  // There should be one unread byte left in the queue at this point.
  // It will be read once the microtask queue is drained.
  KJ_ASSERT(queue.size() == 1);

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ByteQueue with multiple default consumers with atLeast (same rate)") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ByteQueue queue(2);

  ByteQueue::Consumer consumer1(queue);
  ByteQueue::Consumer consumer2(queue);

  const auto read = [&](jsg::Lock& js, auto& consumer, uint atLeast = 1) {
    auto prp = js.newPromiseAndResolver<ReadResult>();
    consumer.read(js, ByteQueue::ReadRequest {
      .resolver = kj::mv(prp.resolver),
      .pullInto = {
        .store = jsg::BackingStore::alloc(js, 5),
        .atLeast = atLeast,
      },
    });
    return kj::mv(prp.promise);
  };

  const auto push = [&](auto store) {
    try {
      queue.push(js, kj::refcounted<ByteQueue::Entry>(kj::mv(store)));
    } catch (kj::Exception& ex) {
      KJ_DBG(ex.getDescription());
    }
  };

  MustCall<ReadContinuation> read1Continuation([&](jsg::Lock& js, auto&& result) {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    auto view = value.getHandle(js);
    KJ_ASSERT(view->IsArrayBufferView());
    jsg::BufferSource source(js, view);
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(ptr[0] == 1);
    KJ_ASSERT(ptr[1] == 2);
    KJ_ASSERT(ptr[2] == 3);
    KJ_ASSERT(ptr[3] == 4);
    KJ_ASSERT(ptr[4] == 5);
    KJ_ASSERT(source.size(), 5);
    KJ_ASSERT(consumer1.size(), 1);
    return read(js, consumer1);
  });

  MustCall<ReadContinuation> read2Continuation([&](jsg::Lock& js, auto&& result) {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    auto view = value.getHandle(js);
    KJ_ASSERT(view->IsArrayBufferView());
    jsg::BufferSource source(js, view);
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(ptr[0] == 1);
    KJ_ASSERT(ptr[1] == 2);
    KJ_ASSERT(ptr[2] == 3);
    KJ_ASSERT(ptr[3] == 4);
    KJ_ASSERT(ptr[4] == 5);
    KJ_ASSERT(source.size(), 5);
    KJ_ASSERT(consumer2.size(), 1);
    return read(js, consumer2);
  });

  MustCall<ReadContinuation> readFinalContinuation([&](jsg::Lock& js, auto&& result) {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    auto view = value.getHandle(js);
    KJ_ASSERT(view->IsArrayBufferView());
    jsg::BufferSource source(js, view);
    KJ_ASSERT(source.asArrayPtr()[0], 6);
    KJ_ASSERT(source.size() == 1);
    return js.resolvedPromise(kj::mv(result));
  }, 2);

  read(js, consumer1, 5).then(js, read1Continuation)
                        .then(js, readFinalContinuation);
  read(js, consumer2, 5).then(js, read2Continuation)
                        .then(js, readFinalContinuation);

  auto store1 = jsg::BackingStore::alloc(js, 2);
  store1.asArrayPtr()[0] = 1;
  store1.asArrayPtr()[1] = 2;
  push(kj::mv(store1));

  KJ_ASSERT(queue.desiredSize() == 0);

  auto store2 = jsg::BackingStore::alloc(js, 2);
  store2.asArrayPtr()[0] = 3;
  store2.asArrayPtr()[1] = 4;
  push(kj::mv(store2));

  // Backpressure should be accumulating because the read has not yet fullilled.
  KJ_ASSERT(queue.desiredSize() == -2);

  auto store3 = jsg::BackingStore::alloc(js, 2);
  store3.asArrayPtr()[0] = 5;
  store3.asArrayPtr()[1] = 6;
  push(kj::mv(store3));

  // Some backpressure should be released because pushing the final minimum
  // amount into the queue should have caused the read to be fulfilled.
  KJ_ASSERT(queue.desiredSize() == 1);

  // There should be one unread byte left in the queue at this point.
  // It will be read once the microtask queue is drained.
  KJ_ASSERT(queue.size() == 1);

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

KJ_TEST("ByteQueue with multiple default consumers with atLeast (different rate)") {
  Preamble preamble;
  auto& js = preamble.getJs();

  ByteQueue queue(2);

  ByteQueue::Consumer consumer1(queue);
  ByteQueue::Consumer consumer2(queue);

  const auto read = [&](jsg::Lock& js, auto& consumer, uint atLeast = 1) {
    auto prp = js.newPromiseAndResolver<ReadResult>();
    consumer.read(js, ByteQueue::ReadRequest {
      .resolver = kj::mv(prp.resolver),
      .pullInto = {
        .store = jsg::BackingStore::alloc(js, 5),
        .atLeast = atLeast,
      },
    });
    return kj::mv(prp.promise);
  };

  const auto push = [&](auto store) {
    try {
      queue.push(js, kj::refcounted<ByteQueue::Entry>(kj::mv(store)));
    } catch (kj::Exception& ex) {
      KJ_DBG(ex.getDescription());
    }
  };

  MustCall<ReadContinuation> read1Continuation([&](jsg::Lock& js, auto&& result) {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    auto view = value.getHandle(js);
    KJ_ASSERT(view->IsArrayBufferView());
    jsg::BufferSource source(js, view);
    KJ_ASSERT(source.size() == 4);
    auto ptr = source.asArrayPtr();
    // Our read was for at least 3 bytes, with a maximun of 5.
    // For this first read, we received 4. One the second read
    // we should receive 2.
    KJ_ASSERT(ptr[0] == 1);
    KJ_ASSERT(ptr[1] == 2);
    KJ_ASSERT(ptr[2] == 3);
    KJ_ASSERT(ptr[3] == 4);
    return js.resolvedPromise(kj::mv(result));
  });

  MustCall<ReadContinuation> read1FinalContinuation([&](jsg::Lock& js, auto&& result) {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    auto view = value.getHandle(js);
    KJ_ASSERT(view->IsArrayBufferView());
    jsg::BufferSource source(js, view);
    KJ_ASSERT(source.size() == 2);
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(ptr[0] == 5);
    KJ_ASSERT(ptr[1] == 6);
    return js.resolvedPromise(kj::mv(result));
  });

  MustCall<ReadContinuation> read2Continuation([&](jsg::Lock& js, auto&& result) {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    auto view = value.getHandle(js);
    KJ_ASSERT(view->IsArrayBufferView());
    jsg::BufferSource source(js, view);
    auto ptr = source.asArrayPtr();
    KJ_ASSERT(source.size() == 5);
    KJ_ASSERT(ptr[0] == 1);
    KJ_ASSERT(ptr[1] == 2);
    KJ_ASSERT(ptr[2] == 3);
    KJ_ASSERT(ptr[3] == 4);
    KJ_ASSERT(ptr[4] == 5);
    KJ_ASSERT(consumer2.size() == 1);
    return read(js, consumer2);
  });

  MustCall<ReadContinuation> read2FinalContinuation([&](jsg::Lock& js, auto&& result) {
    KJ_ASSERT(!result.done);
    auto& value = KJ_ASSERT_NONNULL(result.value);
    auto view = value.getHandle(js);
    KJ_ASSERT(view->IsArrayBufferView());
    jsg::BufferSource source(js, view);
    KJ_ASSERT(source.asArrayPtr()[0] == 6);
    KJ_ASSERT(source.size() == 1);
    return js.resolvedPromise(kj::mv(result));
  });

  // Consumer 1 will read in parallel with smaller minimum chunks...
  read(js, consumer1, 3).then(js, read1Continuation);
  read(js, consumer1).then(js, read1FinalContinuation);

  // Consumer 2 will read serially with a larger minimum chunk...
  read(js, consumer2, 5).then(js, read2Continuation)
                        .then(js, read2FinalContinuation);

  auto store1 = jsg::BackingStore::alloc(js, 2);
  store1.asArrayPtr()[0] = 1;
  store1.asArrayPtr()[1] = 2;
  push(kj::mv(store1));

  KJ_ASSERT(queue.desiredSize() == 0);

  auto store2 = jsg::BackingStore::alloc(js, 2);
  store2.asArrayPtr()[0] = 3;
  store2.asArrayPtr()[1] = 4;
  push(kj::mv(store2));

  // Consumer1 should not have any data buffered since its first read was for
  // between 3 and 5 bytes and it has received four so far.
  KJ_ASSERT(consumer1.size() == 0);

  // Consumer2 should have 4 bytes buffered since its first read was for 5 bytes
  // and we've only received 4 so far.
  KJ_ASSERT(consumer2.size() == 4);

  // Queue backpressure should reflect that consumer2 has data buffered.
  KJ_ASSERT(queue.desiredSize() == -2);

  auto store3 = jsg::BackingStore::alloc(js, 2);
  store3.asArrayPtr()[0] = 5;
  store3.asArrayPtr()[1] = 6;
  push(kj::mv(store3));

  // Most of the backpressure should have been resolved since we delivered 5 bytes
  // to consumer2, but there's still one byte remaining.
  KJ_ASSERT(queue.desiredSize() = 1);
  KJ_ASSERT(queue.size() == 1);

  js.v8Isolate->PerformMicrotaskCheckpoint();
}

#pragma endregion ByteQueue Tests

}  // namespace
}  // namespace workerd::api
