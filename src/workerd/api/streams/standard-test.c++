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
  return jsg::BackingStore::from(str.asBytes().attach(kj::mv(str))).createHandle(js);
}

jsg::BufferSource toBufferSource(jsg::Lock& js, kj::String str) {
  auto backing = jsg::BackingStore::from(str.asBytes().attach(kj::mv(str))).createHandle(js);
  return jsg::BufferSource(js, kj::mv(backing));
}

jsg::BufferSource toBufferSource(jsg::Lock& js, kj::Array<kj::byte> bytes) {
  auto backing = jsg::BackingStore::from(kj::mv(bytes)).createHandle(js);
  return jsg::BufferSource(js, kj::mv(backing));
}

// ======================================================================================
// Happy Cases

KJ_TEST("ReadableStream read all text (value readable)") {
  preamble([](jsg::Lock& js) {
    uint checked = 0;
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js,
        UnderlyingSource{
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
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
        },
        StreamQueuingStrategy{.highWaterMark = 0});

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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js,
        UnderlyingSource{
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
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
        },
        StreamQueuingStrategy{.highWaterMark = 0});

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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js,
        UnderlyingSource{
          .type = kj::str("bytes"),
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
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
        },
        StreamQueuingStrategy{.highWaterMark = 0});

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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js,
        UnderlyingSource{
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
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
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(
        js, [&](jsg::Lock& js, kj::Array<kj::byte>&& text) {
      KJ_ASSERT(text == "Hello, world!"_kjc.asBytes());
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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js,
        UnderlyingSource{
          .type = kj::str("bytes"),
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
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
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(
        js, [&](jsg::Lock& js, kj::Array<kj::byte>&& text) {
      KJ_ASSERT(text == "Hello, world!"_kjc.asBytes());
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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    auto chunks = kj::arr<kj::String>(kj::str("H"), kj::str("e"), kj::str("l"), kj::str("l"),
        kj::str("o"), kj::str(","), kj::str(" "), kj::str("w"), kj::str("o"), kj::str("r"),
        kj::str("l"), kj::str("d"), kj::str("!"));
    rs->getController().setup(js,
        UnderlyingSource{
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
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
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(
        js, [&](jsg::Lock& js, kj::Array<kj::byte>&& text) {
      KJ_ASSERT(text == "Hello, world!"_kjc.asBytes());
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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    auto chunks = kj::arr<kj::String>(kj::str("H"), kj::str("e"), kj::str("l"), kj::str("l"),
        kj::str("o"), kj::str(","), kj::str(" "), kj::str("w"), kj::str("o"), kj::str("r"),
        kj::str("l"), kj::str("d"), kj::str("!"));
    rs->getController().setup(js,
        UnderlyingSource{
          .type = kj::str("bytes"),
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
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
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(
        js, [&](jsg::Lock& js, kj::Array<kj::byte>&& text) {
      KJ_ASSERT(text == "Hello, world!"_kjc.asBytes());
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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    static constexpr uint BASE = 4097;
    auto chunks = kj::arr<kj::Array<kj::byte>>(kj::heapArray<kj::byte>(BASE),
        kj::heapArray<kj::byte>(BASE * 2), kj::heapArray<kj::byte>(BASE * 4));
    chunks[0].asPtr().fill('A');
    chunks[1].asPtr().fill('B');
    chunks[2].asPtr().fill('C');
    rs->getController().setup(js,
        UnderlyingSource{
          .type = kj::str("bytes"),
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
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
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Starts a read loop of javascript promises.
    auto promise = rs->getController()
                       .readAllBytes(js, (BASE * 7) + 1)
                       .then(js, [&](jsg::Lock& js, kj::Array<kj::byte>&& text) {
      kj::byte check[BASE * 7]{};
      kj::arrayPtr(check).first(BASE).fill('A');
      kj::arrayPtr(check).slice(BASE).first(BASE * 2).fill('B');
      kj::arrayPtr(check).slice(BASE * 3).fill('C');
      KJ_ASSERT(text.size() == BASE * 7);
      KJ_ASSERT(check == text);
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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js,
        UnderlyingSource{
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
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
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, kj::Array<kj::byte>&& text) { KJ_UNREACHABLE; },
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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js,
        UnderlyingSource{
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
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
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, kj::Array<kj::byte>&& text) { KJ_UNREACHABLE; },
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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js,
        UnderlyingSource{
          .type = kj::str("bytes"),
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
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
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, kj::Array<kj::byte>&& text) { KJ_UNREACHABLE; },
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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js,
        UnderlyingSource{
          .type = kj::str("bytes"),
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
      checked++;
      return js.rejectedPromise<void>(js.error("boom"));
    }
          // Setting a highWaterMark of 0 means the pull function above will not be called
          // immediately on creation of the stream, but only when the first read in the
          // readall call below happens.
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, kj::Array<kj::byte>&& text) { KJ_UNREACHABLE; },
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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js,
        UnderlyingSource{
          .pull =
              [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
      checked++;
      return js.rejectedPromise<void>(js.error("boom"));
    }
          // Setting a highWaterMark of 0 means the pull function above will not be called
          // immediately on creation of the stream, but only when the first read in the
          // readall call below happens.
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, kj::Array<kj::byte>&& text) { KJ_UNREACHABLE; },
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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js,
        UnderlyingSource{
          .type = kj::str("bytes"),
          .start = [&](jsg::Lock& js,
                       UnderlyingSource::Controller controller) -> jsg::Promise<void> {
      checked++;
      return js.rejectedPromise<void>(js.error("boom"));
    }
          // Setting a highWaterMark of 0 means the pull function above will not be called
          // immediately on creation of the stream, but only when the first read in the
          // readall call below happens.
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, kj::Array<kj::byte>&& text) { KJ_UNREACHABLE; },
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
    auto rs = jsg::alloc<ReadableStream>(newReadableStreamJsController());
    rs->getController().setup(js,
        UnderlyingSource{
          .type = kj::str("bytes"),
          .start = [&](jsg::Lock& js,
                       UnderlyingSource::Controller controller) -> jsg::Promise<void> {
      checked++;
      JSG_FAIL_REQUIRE(Error, "boom");
    }
          // Setting a highWaterMark of 0 means the pull function above will not be called
          // immediately on creation of the stream, but only when the first read in the
          // readall call below happens.
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Starts a read loop of javascript promises.
    auto promise = rs->getController().readAllBytes(js, 20).then(js,
        [](jsg::Lock& js, kj::Array<kj::byte>&& text) { KJ_UNREACHABLE; },
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

}  // namespace
}  // namespace workerd::api
