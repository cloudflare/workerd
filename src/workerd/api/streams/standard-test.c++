#include "readable.h"
#include "standard.h"

#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/observer.h>

namespace workerd::api {
namespace {

jsg::V8System v8System;

struct RsContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(RsContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(RsIsolate, RsContext, ReadResult);

void preamble(auto callback) {
  RsIsolate isolate(v8System, kj::heap<jsg::IsolateObserver>());
  isolate.runInLockScope([&](RsIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(
        lock, lock.newContext<RsContext>().getHandle(lock), [&](jsg::Lock& js) { callback(js); });
  });
}

v8::Local<v8::Value> toBytes(jsg::Lock& js, kj::String str) {
  return jsg::BackingStore::from(js, str.asBytes().attach(kj::mv(str))).createHandle(js);
}

jsg::BufferSource toBufferSource(jsg::Lock& js, kj::String str) {
  auto backing = jsg::BackingStore::from(js, str.asBytes().attach(kj::mv(str))).createHandle(js);
  return jsg::BufferSource(js, kj::mv(backing));
}

jsg::BufferSource toBufferSource(jsg::Lock& js, kj::Array<kj::byte> bytes) {
  auto backing = jsg::BackingStore::from(js, kj::mv(bytes)).createHandle(js);
  return jsg::BufferSource(js, kj::mv(backing));
}

// ======================================================================================
// Happy Cases

KJ_TEST("ReadableStream read all text (value readable)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Because we're using a value-based stream, two enqueue operations will
        // require at least three reads to complete: one for the first chunk, 'hello, ',
        // one for the second chunk, 'world!', and one to signal close.
        KJ_SWITCH_ONEOF(controller) {
          // Because we're using a value-based stream, two enqueue operations will
          // require at least three reads to complete: one for the first chunk, 'hello, ',
          // one for the second chunk, 'world!', and one to signal close.
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            checked++;
            c->enqueue(js, toBytes(js, kj::str("Hello, ")));
            c->enqueue(js, toBytes(js, kj::str("world!")));
            c->close(js);
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise =
        rs->getController().readAllText(js, 20).then(js, [&](jsg::Lock& js, kj::String&& text) {
      KJ_ASSERT(text == "Hello, world!"_kjc);
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 2);

    // Reading everything successfully should cause the stream to close.
    KJ_ASSERT(rs->getController().isClosed());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

KJ_TEST("ReadableStream read all text, rs ref held (value readable)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Because we're using a value-based stream, two enqueue operations will
        // require at least three reads to complete: one for the first chunk, 'hello, ',
        // one for the second chunk, 'world!', and one to signal close.
        KJ_SWITCH_ONEOF(controller) {
          // Because we're using a value-based stream, two enqueue operations will
          // require at least three reads to complete: one for the first chunk, 'hello, ',
          // one for the second chunk, 'world!', and one to signal close.
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            checked++;
            c->enqueue(js, toBytes(js, kj::str("Hello, ")));
            c->enqueue(js, toBytes(js, kj::str("world!")));
            c->close(js);
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise =
        rs->getController().readAllText(js, 20).then(js, [&](jsg::Lock& js, kj::String&& text) {
      KJ_ASSERT(text == "Hello, world!"_kjc);
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Let's drop our ref to rs, things should still work as expected.
    { auto drop = kj::mv(rs); }

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 2);
  });
}

KJ_TEST("ReadableStream read all text (byte readable)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Because we're using a value-based stream, two enqueue operations will
        // require at least three reads to complete: one for the first chunk, 'hello, ',
        // one for the second chunk, 'world!', and one to signal close.
        KJ_SWITCH_ONEOF(controller) {
          // Because we're using a value-based stream, two enqueue operations will
          // require at least three reads to complete: one for the first chunk, 'hello, ',
          // one for the second chunk, 'world!', and one to signal close.
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {
            checked++;
            c->enqueue(js, toBufferSource(js, kj::str("Hello, ")));
            c->enqueue(js, toBufferSource(js, kj::str("world!")));
            c->close(js);
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {}
        }
        KJ_UNREACHABLE;
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise =
        rs->getController().readAllText(js, 20).then(js, [&](jsg::Lock& js, kj::String&& text) {
      KJ_ASSERT(text == "Hello, world!"_kjc);
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 2);

    // Reading everything successfully should cause the stream to close.
    KJ_ASSERT(rs->getController().isClosed());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

KJ_TEST("ReadableStream read all bytes (value readable)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Because we're using a value-based stream, two enqueue operations will
        // require at least three reads to complete: one for the first chunk, 'hello, ',
        // one for the second chunk, 'world!', and one to signal close.
        KJ_SWITCH_ONEOF(controller) {
          // Because we're using a value-based stream, two enqueue operations will
          // require at least three reads to complete: one for the first chunk, 'hello, ',
          // one for the second chunk, 'world!', and one to signal close.
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            checked++;
            c->enqueue(js, toBytes(js, kj::str("Hello, ")));
            c->enqueue(js, toBytes(js, kj::str("world!")));
            c->close(js);
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(
        js, [&](jsg::Lock& js, jsg::BufferSource&& text) {
      KJ_ASSERT(text.asArrayPtr() == "Hello, world!"_kjb);
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 2);

    // Reading everything successfully should cause the stream to close.
    KJ_ASSERT(rs->getController().isClosed());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

KJ_TEST("ReadableStream read all bytes (byte readable)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Because we're using a value-based stream, two enqueue operations will
        // require at least three reads to complete: one for the first chunk, 'hello, ',
        // one for the second chunk, 'world!', and one to signal close.
        KJ_SWITCH_ONEOF(controller) {
          // Because we're using a value-based stream, two enqueue operations will
          // require at least three reads to complete: one for the first chunk, 'hello, ',
          // one for the second chunk, 'world!', and one to signal close.
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {
            checked++;
            c->enqueue(js, toBufferSource(js, kj::str("Hello, ")));
            c->enqueue(js, toBufferSource(js, kj::str("world!")));
            c->close(js);
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {}
        }
        KJ_UNREACHABLE;
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(
        js, [&](jsg::Lock& js, jsg::BufferSource&& text) {
      KJ_ASSERT(text.asArrayPtr() == "Hello, world!"_kjb);
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 2);

    // Reading everything successfully should cause the stream to close.
    KJ_ASSERT(rs->getController().isClosed());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

KJ_TEST("ReadableStream read all bytes (value readable, more reads)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    uint counter = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    auto chunks = kj::arr<kj::String>(kj::str("H"), kj::str("e"), kj::str("l"), kj::str("l"),
        kj::str("o"), kj::str(","), kj::str(" "), kj::str("w"), kj::str("o"), kj::str("r"),
        kj::str("l"), kj::str("d"), kj::str("!"));
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Because we're using a value-based stream, two enqueue operations will
        // require at least three reads to complete: one for the first chunk, 'hello, ',
        // one for the second chunk, 'world!', and one to signal close.
        KJ_SWITCH_ONEOF(controller) {
          // Because we're using a value-based stream, two enqueue operations will
          // require at least three reads to complete: one for the first chunk, 'hello, ',
          // one for the second chunk, 'world!', and one to signal close.
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            checked++;
            c->enqueue(js, toBytes(js, kj::mv(chunks[counter++])));
            if (counter == chunks.size()) {
              c->close(js);
            }

            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(
        js, [&](jsg::Lock& js, jsg::BufferSource&& text) {
      KJ_ASSERT(text.asArrayPtr() == "Hello, world!"_kjb);
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 14);

    // Reading everything successfully should cause the stream to close.
    KJ_ASSERT(rs->getController().isClosed());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

KJ_TEST("ReadableStream read all bytes (byte readable, more reads)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    uint counter = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    auto chunks = kj::arr<kj::String>(kj::str("H"), kj::str("e"), kj::str("l"), kj::str("l"),
        kj::str("o"), kj::str(","), kj::str(" "), kj::str("w"), kj::str("o"), kj::str("r"),
        kj::str("l"), kj::str("d"), kj::str("!"));
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Because we're using a value-based stream, two enqueue operations will
        // require at least three reads to complete: one for the first chunk, 'hello, ',
        // one for the second chunk, 'world!', and one to signal close.
        KJ_SWITCH_ONEOF(controller) {
          // Because we're using a value-based stream, two enqueue operations will
          // require at least three reads to complete: one for the first chunk, 'hello, ',
          // one for the second chunk, 'world!', and one to signal close.
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {
            checked++;
            c->enqueue(js, toBufferSource(js, kj::mv(chunks[counter++])));
            if (counter == chunks.size()) {
              c->close(js);
            }

            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {}
        }
        KJ_UNREACHABLE;
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(
        js, [&](jsg::Lock& js, jsg::BufferSource&& text) {
      KJ_ASSERT(text.asArrayPtr() == "Hello, world!"_kjb);
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 14);

    // Reading everything successfully should cause the stream to close.
    KJ_ASSERT(rs->getController().isClosed());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

KJ_TEST("ReadableStream read all bytes (byte readable, large data)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    uint counter = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    static constexpr uint BASE = 4097;
    auto chunks = kj::arr<kj::Array<kj::byte>>(kj::heapArray<kj::byte>(BASE),
        kj::heapArray<kj::byte>(BASE * 2), kj::heapArray<kj::byte>(BASE * 4));
    chunks[0].asPtr().fill('A');
    chunks[1].asPtr().fill('B');
    chunks[2].asPtr().fill('C');
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Because we're using a value-based stream, two enqueue operations will
        // require at least three reads to complete: one for the first chunk, 'hello, ',
        // one for the second chunk, 'world!', and one to signal close.
        KJ_SWITCH_ONEOF(controller) {
          // Because we're using a value-based stream, two enqueue operations will
          // require at least three reads to complete: one for the first chunk, 'hello, ',
          // one for the second chunk, 'world!', and one to signal close.
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {
            checked++;
            c->enqueue(js, toBufferSource(js, kj::mv(chunks[counter++])));
            if (counter == chunks.size()) {
              c->close(js);
            }

            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {}
        }
        KJ_UNREACHABLE;
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise = rs->getController()
                       .readAllBytes(js, (BASE * 7) + 1)
                       .then(js, [&](jsg::Lock& js, jsg::BufferSource&& text) {
      kj::byte check[BASE * 7]{};
      kj::arrayPtr(check).first(BASE).fill('A');
      kj::arrayPtr(check).slice(BASE).first(BASE * 2).fill('B');
      kj::arrayPtr(check).slice(BASE * 3).fill('C');
      KJ_ASSERT(text.size() == BASE * 7);
      KJ_ASSERT(text.asArrayPtr() == check);
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 4);

    // Reading everything successfully should cause the stream to close.
    KJ_ASSERT(rs->getController().isClosed());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

// ======================================================================================
// Fail cases

KJ_TEST("ReadableStream read all bytes (value readable, wrong type)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Because we're using a value-based stream, two enqueue operations will
        // require at least three reads to complete: one for the first chunk, 'hello, ',
        // one for the second chunk, 'world!', and one to signal close.
        KJ_SWITCH_ONEOF(controller) {
          // Because we're using a value-based stream, two enqueue operations will
          // require at least three reads to complete: one for the first chunk, 'hello, ',
          // one for the second chunk, 'world!', and one to signal close.
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            c->enqueue(js, js.str("wrong type"_kjc));
            checked++;
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      },
      .cancel = [&](jsg::Lock& js, auto reason) -> jsg::Promise<void> {
        KJ_ASSERT(kj::str(reason) == "TypeError: This ReadableStream did not return bytes.");
        checked++;
        return js.resolvedPromise();
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, jsg::BufferSource&& text) { KJ_UNREACHABLE; },
        [&](jsg::Lock& js, jsg::Value&& exception) {
      KJ_ASSERT(kj::str(exception.getHandle(js)) ==
          "TypeError: This ReadableStream did not return bytes.");
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 3);

    KJ_ASSERT(rs->getController().isClosedOrErrored());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

KJ_TEST("ReadableStream read all bytes (value readable, to many bytes)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Because we're using a value-based stream, two enqueue operations will
        // require at least three reads to complete: one for the first chunk, 'hello, ',
        // one for the second chunk, 'world!', and one to signal close.
        KJ_SWITCH_ONEOF(controller) {
          // Because we're using a value-based stream, two enqueue operations will
          // require at least three reads to complete: one for the first chunk, 'hello, ',
          // one for the second chunk, 'world!', and one to signal close.
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            c->enqueue(js, toBytes(js, kj::str("123456789012345678901")));
            checked++;
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, jsg::BufferSource&& text) { KJ_UNREACHABLE; },
        [&](jsg::Lock& js, jsg::Value&& exception) {
      KJ_ASSERT(kj::str(exception.getHandle(js)) == "TypeError: Memory limit exceeded before EOF.");
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 2);

    KJ_ASSERT(rs->getController().isClosedOrErrored());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

KJ_TEST("ReadableStream read all bytes (byte readable, to many bytes)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Because we're using a value-based stream, two enqueue operations will
        // require at least three reads to complete: one for the first chunk, 'hello, ',
        // one for the second chunk, 'world!', and one to signal close.
        KJ_SWITCH_ONEOF(controller) {
          // Because we're using a value-based stream, two enqueue operations will
          // require at least three reads to complete: one for the first chunk, 'hello, ',
          // one for the second chunk, 'world!', and one to signal close.
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {
            c->enqueue(js, toBufferSource(js, kj::str("123456789012345678901")));
            checked++;
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {}
        }
        KJ_UNREACHABLE;
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, jsg::BufferSource&& text) { KJ_UNREACHABLE; },
        [&](jsg::Lock& js, jsg::Value&& exception) {
      KJ_ASSERT(kj::str(exception.getHandle(js)) == "TypeError: Memory limit exceeded before EOF.");
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 2);

    KJ_ASSERT(rs->getController().isClosedOrErrored());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

KJ_TEST("ReadableStream read all bytes (byte readable, failed read)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        checked++;
        return js.rejectedPromise<void>(js.error("boom"));
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, jsg::BufferSource&& text) { KJ_UNREACHABLE; },
        [&](jsg::Lock& js, jsg::Value&& exception) {
      KJ_ASSERT(kj::str(exception.getHandle(js)) == "Error: boom");
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 2);

    KJ_ASSERT(rs->getController().isClosedOrErrored());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

KJ_TEST("ReadableStream read all bytes (value readable, failed read)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        checked++;
        return js.rejectedPromise<void>(js.error("boom"));
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, jsg::BufferSource&& text) { KJ_UNREACHABLE; },
        [&](jsg::Lock& js, jsg::Value&& exception) {
      KJ_ASSERT(kj::str(exception.getHandle(js)) == "Error: boom");
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 2);

    KJ_ASSERT(rs->getController().isClosedOrErrored());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

KJ_TEST("ReadableStream read all bytes (byte readable, failed start)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .start = [&](jsg::Lock& js, UnderlyingSource::Controller controller) -> jsg::Promise<void> {
        checked++;
        return js.rejectedPromise<void>(js.error("boom"));
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, jsg::BufferSource&& text) { KJ_UNREACHABLE; },
        [&](jsg::Lock& js, jsg::Value&& exception) {
      KJ_ASSERT(kj::str(exception.getHandle(js)) == "Error: boom");
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 2);

    KJ_ASSERT(rs->getController().isClosedOrErrored());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

KJ_TEST("ReadableStream read all bytes (byte readable, failed start 2)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .start = [&](jsg::Lock& js, UnderlyingSource::Controller controller) -> jsg::Promise<void> {
        checked++;
        JSG_FAIL_REQUIRE(Error, "boom");
      }
      // Setting a highWaterMark of 0 means the pull function above will not be called
      // immediately on creation of the stream, but only when the first read in the
      // readall call below happens.
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, jsg::BufferSource&& text) { KJ_UNREACHABLE; },
        [&](jsg::Lock& js, jsg::Value&& exception) {
      KJ_ASSERT(kj::str(exception.getHandle(js)) == "Error: boom");
      checked++;
    });

    // Reading left the stream locked and disturbed
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());

    // Run the microtasks to completion. This should resolve the promise and
    // run it to completion. The test is buggy if it fails to do so.
    js.runMicrotasks();
    KJ_ASSERT(checked == 2);

    KJ_ASSERT(rs->getController().isClosedOrErrored());

    // Add we should still be locked and disturbed.
    KJ_ASSERT(rs->isLocked());
    KJ_ASSERT(rs->isDisturbed());
  });
}

// ======================================================================================
// DrainingReader tests

KJ_TEST("DrainingReader basic creation and locking (value stream)") {
  preamble([](jsg::Lock& js) {
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js, UnderlyingSource{}, StreamQueuingStrategy{.highWaterMark = 0});

    // Stream should not be locked initially
    KJ_ASSERT(!rs->isLocked());

    // Create DrainingReader
    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      // Stream should now be locked
      KJ_ASSERT(rs->isLocked());
      KJ_ASSERT(reader->isAttached());

      // Release the lock
      reader->releaseLock(js);
      KJ_ASSERT(!rs->isLocked());
      KJ_ASSERT(!reader->isAttached());
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader cannot be created on locked stream") {
  preamble([](jsg::Lock& js) {
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js, UnderlyingSource{}, StreamQueuingStrategy{.highWaterMark = 0});

    // Create first reader to lock the stream
    KJ_IF_SOME(reader1, DrainingReader::create(js, *rs)) {
      KJ_ASSERT(rs->isLocked());

      // Try to create another reader - should fail
      auto maybeReader2 = DrainingReader::create(js, *rs);
      KJ_ASSERT(maybeReader2 == kj::none);

      reader1->releaseLock(js);
    } else {
      KJ_FAIL_ASSERT("Failed to create first DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader read drains buffered data (value stream)") {
  preamble([](jsg::Lock& js) {
    uint pullCount = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            pullCount++;
            if (pullCount == 1) {
              // First pull - enqueue multiple chunks
              c->enqueue(js, toBytes(js, kj::str("Hello, ")));
              c->enqueue(js, toBytes(js, kj::str("world!")));
            } else {
              // Second pull - close the stream
              c->close(js);
            }
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      bool readCompleted = false;
      auto promise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        // Should have drained both chunks
        KJ_ASSERT(result.chunks.size() == 2);
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "Hello, ");
        KJ_ASSERT(kj::str(result.chunks[1].asChars()) == "world!");
        KJ_ASSERT(!result.done);  // Stream not closed yet
        readCompleted = true;
      });

      js.runMicrotasks();
      KJ_ASSERT(readCompleted);
      KJ_ASSERT(pullCount == 1);  // Only one pull needed

      reader->releaseLock(js);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader read drains buffered data (byte stream)") {
  preamble([](jsg::Lock& js) {
    uint pullCount = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {}
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {
            pullCount++;
            if (pullCount == 1) {
              c->enqueue(js, toBufferSource(js, kj::str("Hello, ")));
              c->enqueue(js, toBufferSource(js, kj::str("world!")));
            } else {
              c->close(js);
            }
            return js.resolvedPromise();
          }
        }
        KJ_UNREACHABLE;
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      bool readCompleted = false;
      auto promise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        // Should have drained both chunks
        KJ_ASSERT(result.chunks.size() == 2);
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "Hello, ");
        KJ_ASSERT(kj::str(result.chunks[1].asChars()) == "world!");
        KJ_ASSERT(!result.done);
        readCompleted = true;
      });

      js.runMicrotasks();
      KJ_ASSERT(readCompleted);
      KJ_ASSERT(pullCount == 1);

      reader->releaseLock(js);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader read on closed stream returns done") {
  preamble([](jsg::Lock& js) {
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .start = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            c->close(js);
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    js.runMicrotasks();

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      bool readCompleted = false;
      auto promise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        KJ_ASSERT(result.chunks.size() == 0);
        KJ_ASSERT(result.done);
        readCompleted = true;
      });

      js.runMicrotasks();
      KJ_ASSERT(readCompleted);

      reader->releaseLock(js);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader read after releaseLock rejects") {
  preamble([](jsg::Lock& js) {
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js, UnderlyingSource{}, StreamQueuingStrategy{.highWaterMark = 0});

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      reader->releaseLock(js);

      bool readRejected = false;
      auto promise =
          reader->read(js).catch_(js, [&](jsg::Lock& js, jsg::Value reason) -> DrainingReadResult {
        readRejected = true;
        return DrainingReadResult{
          .chunks = kj::Array<kj::Array<kj::byte>>(),
          .done = true,
        };
      });

      js.runMicrotasks();
      KJ_ASSERT(readRejected);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader sync data then async pull waits") {
  // Test case: pull enqueues some data synchronously, then returns a pending promise.
  // The first draining read should get the sync data immediately.
  // A second draining read should wait for the async pull to complete.
  preamble([](jsg::Lock& js) {
    uint pullCount = 0;
    kj::Maybe<jsg::Promise<void>::Resolver> asyncResolver;
    kj::Maybe<jsg::Ref<ReadableStreamDefaultController>> savedController;

    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            pullCount++;
            if (pullCount == 1) {
              // First pull: enqueue data synchronously, but return async promise
              c->enqueue(js, toBytes(js, kj::str("sync-chunk")));
              // Return a promise that resolves later
              auto prp = js.newPromiseAndResolver<void>();
              asyncResolver = kj::mv(prp.resolver);
              savedController = c.addRef();
              return kj::mv(prp.promise);
            } else if (pullCount == 2) {
              // Second pull after async resolution: enqueue more data
              c->enqueue(js, toBytes(js, kj::str("async-chunk")));
              return js.resolvedPromise();
            }
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      // First read - should get sync data immediately
      bool firstReadCompleted = false;
      auto promise1 = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        KJ_ASSERT(result.chunks.size() == 1);
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "sync-chunk");
        KJ_ASSERT(!result.done);
        firstReadCompleted = true;
      });

      js.runMicrotasks();
      KJ_ASSERT(firstReadCompleted);
      KJ_ASSERT(pullCount == 1);  // Only first pull happened

      // Second read - should wait for async data
      bool secondReadCompleted = false;
      auto promise2 = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        // Should get the async chunk
        KJ_ASSERT(result.chunks.size() >= 1);
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "async-chunk");
        KJ_ASSERT(!result.done);
        secondReadCompleted = true;
      });

      js.runMicrotasks();
      // Second read should NOT complete yet - still waiting for async pull
      KJ_ASSERT(!secondReadCompleted);

      // Now resolve the async pull
      KJ_ASSERT_NONNULL(asyncResolver).resolve(js);
      js.runMicrotasks();

      // Now second read should complete
      KJ_ASSERT(secondReadCompleted);
      KJ_ASSERT(pullCount == 2);

      reader->releaseLock(js);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader with fully async pull") {
  // Test case: pull returns a promise without enqueueing anything synchronously.
  // The draining read should wait for the pull to complete and then get the data.
  preamble([](jsg::Lock& js) {
    uint pullCount = 0;
    kj::Maybe<jsg::Promise<void>::Resolver> asyncResolver;
    kj::Maybe<jsg::Ref<ReadableStreamDefaultController>> savedController;

    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            pullCount++;
            // Return a promise and save the controller - will enqueue data when resolved
            auto prp = js.newPromiseAndResolver<void>();
            asyncResolver = kj::mv(prp.resolver);
            savedController = c.addRef();
            return kj::mv(prp.promise);
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      bool readCompleted = false;
      auto promise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        KJ_ASSERT(result.chunks.size() == 1);
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "async-data");
        KJ_ASSERT(!result.done);
        readCompleted = true;
      });

      js.runMicrotasks();
      // Read should NOT complete yet - waiting for async pull
      KJ_ASSERT(!readCompleted);
      KJ_ASSERT(pullCount == 1);

      // Enqueue data and resolve the pull
      KJ_ASSERT_NONNULL(savedController)->enqueue(js, toBytes(js, kj::str("async-data")));
      KJ_ASSERT_NONNULL(asyncResolver).resolve(js);
      js.runMicrotasks();

      // Now read should complete
      KJ_ASSERT(readCompleted);

      reader->releaseLock(js);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader byte stream with async pull") {
  // Test async behavior with byte streams
  preamble([](jsg::Lock& js) {
    uint pullCount = 0;
    kj::Maybe<jsg::Promise<void>::Resolver> asyncResolver;
    kj::Maybe<jsg::Ref<ReadableByteStreamController>> savedController;

    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {}
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {
            pullCount++;
            if (pullCount == 1) {
              // Enqueue sync data but return async
              c->enqueue(js, toBufferSource(js, kj::str("sync-bytes")));
              auto prp = js.newPromiseAndResolver<void>();
              asyncResolver = kj::mv(prp.resolver);
              savedController = c.addRef();
              return kj::mv(prp.promise);
            }
            return js.resolvedPromise();
          }
        }
        KJ_UNREACHABLE;
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      // First read gets sync data
      bool firstReadCompleted = false;
      auto promise1 = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        KJ_ASSERT(result.chunks.size() == 1);
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "sync-bytes");
        KJ_ASSERT(!result.done);
        firstReadCompleted = true;
      });

      js.runMicrotasks();
      KJ_ASSERT(firstReadCompleted);

      // Resolve async pull to allow future pulls
      KJ_ASSERT_NONNULL(asyncResolver).resolve(js);
      js.runMicrotasks();

      reader->releaseLock(js);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader multiple sync chunks then close") {
  // Test: Multiple sync chunks followed by close in the same pull
  preamble([](jsg::Lock& js) {
    uint pullCount = 0;
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            pullCount++;
            // Enqueue multiple chunks then close
            c->enqueue(js, toBytes(js, kj::str("chunk1")));
            c->enqueue(js, toBytes(js, kj::str("chunk2")));
            c->enqueue(js, toBytes(js, kj::str("chunk3")));
            c->close(js);
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      bool readCompleted = false;
      auto promise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        // Should get all 3 chunks and done=true
        KJ_ASSERT(result.chunks.size() == 3);
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "chunk1");
        KJ_ASSERT(kj::str(result.chunks[1].asChars()) == "chunk2");
        KJ_ASSERT(kj::str(result.chunks[2].asChars()) == "chunk3");
        KJ_ASSERT(result.done);
        readCompleted = true;
      });

      js.runMicrotasks();
      KJ_ASSERT(readCompleted);
      KJ_ASSERT(pullCount == 1);

      reader->releaseLock(js);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

}  // namespace
}  // namespace workerd::api
