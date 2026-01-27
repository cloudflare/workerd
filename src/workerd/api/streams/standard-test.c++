#include "readable.h"
#include "standard.h"

#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/observer.h>
#include <workerd/tests/test-fixture.h>

namespace workerd::api {
namespace {

void preamble(auto callback) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) { callback(env.js); });
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

KJ_TEST("DrainingReader read from teed branches") {
  // Test: DrainingReader works correctly on both branches of a teed stream
  preamble([](jsg::Lock& js) {
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            c->enqueue(js, toBytes(js, kj::str("chunk1")));
            c->enqueue(js, toBytes(js, kj::str("chunk2")));
            c->close(js);
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Tee the stream into two branches
    auto branches = rs->tee(js);
    KJ_ASSERT(branches.size() == 2);
    auto& branch1 = branches[0];
    auto& branch2 = branches[1];

    // Create DrainingReader on branch1
    KJ_IF_SOME(reader1, DrainingReader::create(js, *branch1)) {
      bool read1Completed = false;
      auto promise1 = reader1->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        KJ_ASSERT(result.chunks.size() == 2);
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "chunk1");
        KJ_ASSERT(kj::str(result.chunks[1].asChars()) == "chunk2");
        KJ_ASSERT(result.done);
        read1Completed = true;
      });

      js.runMicrotasks();
      KJ_ASSERT(read1Completed, "Branch1 read should complete");

      reader1->releaseLock(js);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader for branch1");
    }

    // Create DrainingReader on branch2 - should get the same data
    KJ_IF_SOME(reader2, DrainingReader::create(js, *branch2)) {
      bool read2Completed = false;
      auto promise2 = reader2->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        KJ_ASSERT(result.chunks.size() == 2);
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "chunk1");
        KJ_ASSERT(kj::str(result.chunks[1].asChars()) == "chunk2");
        KJ_ASSERT(result.done);
        read2Completed = true;
      });

      js.runMicrotasks();
      KJ_ASSERT(read2Completed, "Branch2 read should complete");

      reader2->releaseLock(js);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader for branch2");
    }
  });
}

KJ_TEST("DrainingReader read from byte stream with BYOB support") {
  // Test: DrainingReader works correctly with byte streams (which support BYOB reads)
  // even though DrainingReader itself uses a default reader. This tests that closing
  // the controller synchronously during draining works correctly - the controller's
  // close triggers doClose() which must be deferred while onConsumerWantsData is active.
  preamble([](jsg::Lock& js) {
    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {}
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {
            // Enqueue multiple byte chunks - verifies DrainingReader handles
            // byte stream chunks correctly and preserves order
            c->enqueue(js, toBufferSource(js, kj::str("byob-chunk1")));
            c->enqueue(js, toBufferSource(js, kj::str("byob-chunk2")));
            c->enqueue(js, toBufferSource(js, kj::str("byob-chunk3")));
            // Close synchronously - this tests that the fix for use-after-free works.
            // Without the fix, this would cause ByteReadable to be destroyed while
            // onConsumerWantsData is still on the stack.
            c->close(js);
            return js.resolvedPromise();
          }
        }
        KJ_UNREACHABLE;
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    // Use DrainingReader (which uses default reader) to drain the BYOB-capable byte stream
    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      bool readCompleted = false;
      auto promise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        // Should successfully drain all byte chunks in order
        KJ_ASSERT(result.chunks.size() == 3, "Should get 3 chunks");
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "byob-chunk1");
        KJ_ASSERT(kj::str(result.chunks[1].asChars()) == "byob-chunk2");
        KJ_ASSERT(kj::str(result.chunks[2].asChars()) == "byob-chunk3");
        KJ_ASSERT(result.done);  // Stream closed, so done should be true
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

KJ_TEST("DrainingReader read from stream with transform-like pattern") {
  // Test: DrainingReader works correctly with a stream that simulates the
  // TransformStream pattern where data is written to writable and read from readable
  preamble([](jsg::Lock& js) {
    // Create a stream where the controller is stored and chunks are enqueued asynchronously
    // (simulating how TransformStream's readable side receives transformed data)
    kj::Maybe<jsg::Ref<ReadableStreamDefaultController>> savedController;
    bool startResolved = false;

    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .start = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            savedController = c.addRef();
            startResolved = true;
            return js.resolvedPromise();
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      },
      .pull = [](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // No-op pull - data comes from external enqueue calls (like transform writes)
        return js.resolvedPromise();
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    js.runMicrotasks();
    KJ_ASSERT(startResolved, "Stream should have started");

    auto& controller = KJ_ASSERT_NONNULL(savedController, "Controller should be saved");

    // Simulate TransformStream write->transform->enqueue pattern
    // Enqueue transformed chunks (like what TransformStream's transform callback would do)
    controller->enqueue(js, toBytes(js, kj::str("transformed-a")));
    controller->enqueue(js, toBytes(js, kj::str("transformed-b")));

    // Create DrainingReader to drain all buffered transformed data
    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      bool readCompleted = false;
      auto promise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        // Should drain all enqueued chunks
        KJ_ASSERT(result.chunks.size() == 2);
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "transformed-a");
        KJ_ASSERT(kj::str(result.chunks[1].asChars()) == "transformed-b");
        KJ_ASSERT(!result.done);  // Stream not closed yet
        readCompleted = true;
      });

      js.runMicrotasks();
      KJ_ASSERT(readCompleted);

      // Simulate more data being written/transformed
      controller->enqueue(js, toBytes(js, kj::str("transformed-c")));
      controller->close(js);

      bool finalReadCompleted = false;
      auto finalPromise =
          reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        KJ_ASSERT(result.chunks.size() == 1);
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "transformed-c");
        KJ_ASSERT(result.done);  // Stream now closed
        finalReadCompleted = true;
      });

      js.runMicrotasks();
      KJ_ASSERT(finalReadCompleted);

      reader->releaseLock(js);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader cancel while read is pending (value stream)") {
  // Test: Calling cancel() on the reader while a read() is pending should
  // cause the pending read to reject and the stream to be canceled.
  preamble([](jsg::Lock& js) {
    kj::Maybe<jsg::Promise<void>::Resolver> asyncResolver;
    bool cancelCalled = false;

    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Return a pending promise to keep the read waiting
        auto prp = js.newPromiseAndResolver<void>();
        asyncResolver = kj::mv(prp.resolver);
        return kj::mv(prp.promise);
      },
      .cancel = [&](jsg::Lock& js, auto reason) -> jsg::Promise<void> {
        cancelCalled = true;
        KJ_ASSERT(kj::str(reason) == "canceled by reader");
        return js.resolvedPromise();
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      // Start a read that will be pending (waiting for async pull)
      bool readRejected = false;
      bool readResolved = false;
      auto readPromise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        readResolved = true;
      }, [&](jsg::Lock& js, jsg::Value&& reason) { readRejected = true; });

      js.runMicrotasks();
      // Read should still be pending - waiting for async pull
      KJ_ASSERT(!readResolved);
      KJ_ASSERT(!readRejected);

      // Now cancel while read is pending
      bool cancelResolved = false;
      auto cancelPromise =
          reader->cancel(js, js.str("canceled by reader"_kjc)).then(js, [&](jsg::Lock& js) {
        cancelResolved = true;
      });

      js.runMicrotasks();

      // Cancel should have completed
      KJ_ASSERT(cancelResolved, "cancel() should resolve");
      KJ_ASSERT(cancelCalled, "underlying source cancel should be called");

      // The pending read should have been rejected or resolved with done
      KJ_ASSERT(readResolved || readRejected, "pending read should complete after cancel");

      // Stream should be in closed/errored state
      KJ_ASSERT(rs->getController().isClosedOrErrored());
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader cancel while read is pending (byte stream)") {
  // Test: Same as above but with byte stream
  preamble([](jsg::Lock& js) {
    kj::Maybe<jsg::Promise<void>::Resolver> asyncResolver;
    bool cancelCalled = false;

    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        // Return a pending promise to keep the read waiting
        auto prp = js.newPromiseAndResolver<void>();
        asyncResolver = kj::mv(prp.resolver);
        return kj::mv(prp.promise);
      },
      .cancel = [&](jsg::Lock& js, auto reason) -> jsg::Promise<void> {
        cancelCalled = true;
        KJ_ASSERT(kj::str(reason) == "canceled by reader");
        return js.resolvedPromise();
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      // Start a read that will be pending
      bool readRejected = false;
      bool readResolved = false;
      auto readPromise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        readResolved = true;
      }, [&](jsg::Lock& js, jsg::Value&& reason) { readRejected = true; });

      js.runMicrotasks();
      KJ_ASSERT(!readResolved);
      KJ_ASSERT(!readRejected);

      // Cancel while read is pending
      bool cancelResolved = false;
      auto cancelPromise =
          reader->cancel(js, js.str("canceled by reader"_kjc)).then(js, [&](jsg::Lock& js) {
        cancelResolved = true;
      });

      js.runMicrotasks();

      KJ_ASSERT(cancelResolved, "cancel() should resolve");
      KJ_ASSERT(cancelCalled, "underlying source cancel should be called");
      KJ_ASSERT(readResolved || readRejected, "pending read should complete after cancel");
      KJ_ASSERT(rs->getController().isClosedOrErrored());
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader cancel while read is pending with buffered data") {
  // Test: Cancel while read is pending, but there's already some buffered data.
  // The buffered data should be discarded and the stream canceled.
  preamble([](jsg::Lock& js) {
    kj::Maybe<jsg::Promise<void>::Resolver> asyncResolver;
    kj::Maybe<jsg::Ref<ReadableStreamDefaultController>> savedController;
    bool cancelCalled = false;

    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        KJ_SWITCH_ONEOF(controller) {
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableStreamDefaultController>) {
            // Enqueue some data synchronously
            c->enqueue(js, toBytes(js, kj::str("buffered-data")));
            savedController = c.addRef();
            // But return a pending promise (more data coming)
            auto prp = js.newPromiseAndResolver<void>();
            asyncResolver = kj::mv(prp.resolver);
            return kj::mv(prp.promise);
          }
          KJ_CASE_ONEOF(c, jsg::Ref<ReadableByteStreamController>) {}
        }
        KJ_UNREACHABLE;
      },
      .cancel = [&](jsg::Lock& js, auto reason) -> jsg::Promise<void> {
        cancelCalled = true;
        return js.resolvedPromise();
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      // First read gets the buffered data
      bool firstReadCompleted = false;
      auto readPromise1 =
          reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        KJ_ASSERT(result.chunks.size() == 1);
        KJ_ASSERT(kj::str(result.chunks[0].asChars()) == "buffered-data");
        KJ_ASSERT(!result.done);
        firstReadCompleted = true;
      });

      js.runMicrotasks();
      KJ_ASSERT(firstReadCompleted);

      // Second read will be pending (waiting for async pull resolution)
      bool secondReadRejected = false;
      bool secondReadResolved = false;
      auto readPromise2 = reader->read(js).then(js,
          [&](jsg::Lock& js, DrainingReadResult&& result) { secondReadResolved = true; },
          [&](jsg::Lock& js, jsg::Value&& reason) { secondReadRejected = true; });

      js.runMicrotasks();
      KJ_ASSERT(!secondReadResolved);
      KJ_ASSERT(!secondReadRejected);

      // Cancel while second read is pending
      bool cancelResolved = false;
      auto cancelPromise =
          reader->cancel(js, js.str("cancel reason"_kjc)).then(js, [&](jsg::Lock& js) {
        cancelResolved = true;
      });

      js.runMicrotasks();

      KJ_ASSERT(cancelResolved);
      KJ_ASSERT(cancelCalled);
      KJ_ASSERT(secondReadResolved || secondReadRejected);
      KJ_ASSERT(rs->getController().isClosedOrErrored());
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader cancel while read pending - UAF safety (value stream)") {
  // This test specifically exercises the potential UAF scenario where:
  // 1. A draining read creates a promise with lambdas capturing `this` (the Consumer)
  // 2. Cancel is called, which rejects the pending read (scheduling the error lambda)
  // 3. doClose() destroys the Consumer
  // 4. The error lambda runs and must NOT access the destroyed Consumer
  //
  // The lambdas in ValueQueue::Consumer::drainingRead capture `this` to clear
  // hasPendingDrainingRead. If the Consumer is destroyed before the lambda runs,
  // this would be a use-after-free.
  preamble([](jsg::Lock& js) {
    kj::Maybe<jsg::Promise<void>::Resolver> asyncResolver;
    bool cancelCalled = false;
    bool pullCalled = false;

    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        pullCalled = true;
        // Return a pending promise - this keeps the read waiting
        auto prp = js.newPromiseAndResolver<void>();
        asyncResolver = kj::mv(prp.resolver);
        return kj::mv(prp.promise);
      },
      .cancel = [&](jsg::Lock& js, auto reason) -> jsg::Promise<void> {
        cancelCalled = true;
        return js.resolvedPromise();
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      // Start a draining read - this will:
      // 1. Call pull (which returns pending promise)
      // 2. Queue a ReadRequest
      // 3. Return a promise with lambdas capturing `this` (the Consumer)
      bool readRejected = false;
      bool readResolved = false;
      auto readPromise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        readResolved = true;
      }, [&](jsg::Lock& js, jsg::Value&& reason) {
        // This error handler runs after cancel rejects the pending read.
        // The lambda in drainingRead also runs to clear hasPendingDrainingRead.
        // If that lambda accesses a destroyed Consumer, we have UAF.
        readRejected = true;
      });

      js.runMicrotasks();
      KJ_ASSERT(pullCalled, "pull should have been called");
      KJ_ASSERT(!readResolved);
      KJ_ASSERT(!readRejected);

      // Now cancel. This will:
      // 1. Call cancelPendingReads() which rejects the ReadRequest
      // 2. The rejection schedules the error lambda as a microtask
      // 3. doClose() runs (via KJ_DEFER) and may destroy the Consumer
      // 4. Microtasks run - the error lambda in drainingRead accesses `this`
      //
      // If `this` is destroyed before the lambda runs, we have UAF.
      bool cancelResolved = false;
      auto cancelPromise =
          reader->cancel(js, js.str("cancel for UAF test"_kjc)).then(js, [&](jsg::Lock& js) {
        cancelResolved = true;
      });

      // Run microtasks - this is where UAF would occur if the bug exists
      js.runMicrotasks();

      KJ_ASSERT(cancelResolved, "cancel should resolve");
      KJ_ASSERT(cancelCalled, "underlying source cancel should be called");
      KJ_ASSERT(readResolved || readRejected, "read should complete after cancel");
      KJ_ASSERT(rs->getController().isClosedOrErrored(), "stream should be closed/errored");
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader cancel while read pending - UAF safety (byte stream)") {
  // Same test as above but for byte streams (ByteQueue::Consumer)
  preamble([](jsg::Lock& js) {
    kj::Maybe<jsg::Promise<void>::Resolver> asyncResolver;
    bool cancelCalled = false;
    bool pullCalled = false;

    auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
    // clang-format off
    rs->getController().setup(js, UnderlyingSource{
      .type = kj::str("bytes"),
      .pull = [&](jsg::Lock& js, UnderlyingSource::Controller controller) {
        pullCalled = true;
        auto prp = js.newPromiseAndResolver<void>();
        asyncResolver = kj::mv(prp.resolver);
        return kj::mv(prp.promise);
      },
      .cancel = [&](jsg::Lock& js, auto reason) -> jsg::Promise<void> {
        cancelCalled = true;
        return js.resolvedPromise();
      }
    }, StreamQueuingStrategy{.highWaterMark = 0});
    // clang-format on

    KJ_IF_SOME(reader, DrainingReader::create(js, *rs)) {
      bool readRejected = false;
      bool readResolved = false;
      auto readPromise = reader->read(js).then(js, [&](jsg::Lock& js, DrainingReadResult&& result) {
        readResolved = true;
      }, [&](jsg::Lock& js, jsg::Value&& reason) { readRejected = true; });

      js.runMicrotasks();
      KJ_ASSERT(pullCalled);
      KJ_ASSERT(!readResolved);
      KJ_ASSERT(!readRejected);

      bool cancelResolved = false;
      auto cancelPromise =
          reader->cancel(js, js.str("cancel for UAF test"_kjc)).then(js, [&](jsg::Lock& js) {
        cancelResolved = true;
      });

      js.runMicrotasks();

      KJ_ASSERT(cancelResolved);
      KJ_ASSERT(cancelCalled);
      KJ_ASSERT(readResolved || readRejected);
      KJ_ASSERT(rs->getController().isClosedOrErrored());
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

}  // namespace
}  // namespace workerd::api
