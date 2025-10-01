#include "standard.h"
#include "writable-sink-adapter.h"
#include "writable.h"

#include <workerd/api/system-streams.h>
#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/own-util.h>
#include <workerd/util/stream-utils.h>

namespace workerd::api::streams {

namespace {
struct SimpleEventRecordingSink final: public WritableStreamSink {
  struct State {
    size_t writeCalled = 0;
    bool endCalled = false;
    bool abortCalled = false;
  };
  State state;
  SimpleEventRecordingSink() = default;

  State& getState() {
    return state;
  }

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    state.writeCalled++;
    return kj::READY_NOW;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    state.writeCalled++;
    return kj::READY_NOW;
  }
  kj::Promise<void> end() override {
    state.endCalled = true;
    return kj::READY_NOW;
  }
  void abort(kj::Exception reason) override {
    state.abortCalled = true;
  }
};

struct NeverReadySink final: public WritableStreamSink {
  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return kj::NEVER_DONE;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return kj::NEVER_DONE;
  }
  kj::Promise<void> end() override {
    return kj::NEVER_DONE;
  }
  void abort(kj::Exception reason) override {}
};

struct ThrowingSink final: public WritableStreamSink {
  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return KJ_EXCEPTION(FAILED, "worker_do_not_log; write() always throws");
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return KJ_EXCEPTION(FAILED, "worker_do_not_log; write() always throws");
  }
  kj::Promise<void> end() override {
    return KJ_EXCEPTION(FAILED, "worker_do_not_log; end() always throws");
  }
  void abort(kj::Exception reason) override {}
};
}  // namespace

KJ_TEST("Basic construction with default options") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullOutputStream = newNullOutputStream();
    auto sink = newSystemStream(kj::mv(nullOutputStream), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(sink));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(!adapter->isClosing(), "Adapter should not be closing upon construction");
    KJ_ASSERT(adapter->isErrored() == kj::none, "Adapter should not be errored upon construction");
    KJ_ASSERT(KJ_ASSERT_NONNULL(adapter->getDesiredSize()) == 16384,
        "Adapter should have default highWaterMark of 16384");
    auto& options = KJ_ASSERT_NONNULL(adapter->getOptions());
    KJ_ASSERT(options.highWaterMark == 16384);
    KJ_ASSERT(options.detachOnWrite == false);

    auto readyPromise = adapter->getReady(env.js);
    KJ_ASSERT(readyPromise.getState(env.js) == jsg::Promise<void>::State::FULFILLED,
        "Initial ready promise should be fulfilled");
  });
}

KJ_TEST("Construction with custom highWaterMark option") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullOutputStream = newNullOutputStream();
    auto sink = newSystemStream(kj::mv(nullOutputStream), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(sink),
        WritableStreamSinkJsAdapter::Options{.highWaterMark = 100});

    KJ_ASSERT(KJ_ASSERT_NONNULL(adapter->getDesiredSize()) == 100,
        "Adapter should have custom highWaterMark of 100");
  });
}

KJ_TEST("Construction with detachOnWrite=true option") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullOutputStream = newNullOutputStream();
    auto sink = newSystemStream(kj::mv(nullOutputStream), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(sink),
        WritableStreamSinkJsAdapter::Options{
          .detachOnWrite = true,
        });
    auto& options = KJ_ASSERT_NONNULL(adapter->getOptions());
    KJ_ASSERT(options.detachOnWrite == true);
  });
}

KJ_TEST("Construction with all custom options combined") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullOutputStream = newNullOutputStream();
    auto sink = newSystemStream(kj::mv(nullOutputStream), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(sink),
        WritableStreamSinkJsAdapter::Options{
          .highWaterMark = 100,
          .detachOnWrite = true,
        });
    auto& options = KJ_ASSERT_NONNULL(adapter->getOptions());
    KJ_ASSERT(options.highWaterMark == 100);
    KJ_ASSERT(options.detachOnWrite == true);
  });
}

KJ_TEST("Basic end() operation completes successfully") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullOutputStream = newNullOutputStream();
    auto sink = newSystemStream(kj::mv(nullOutputStream), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(sink));

    auto endPromise = adapter->end(env.js);

    KJ_ASSERT(endPromise.getState(env.js) == jsg::Promise<void>::State::PENDING,
        "End promise should be pending immediately after end() call");
    KJ_ASSERT(
        adapter->isClosed() == false, "Adapter should not be closed immediately after end() call");
    KJ_ASSERT(adapter->isClosing() == true,
        "Adapter should be in closing state immediately after end() call");
    KJ_ASSERT(adapter->isErrored() == kj::none, "Adapter should not be errored after end() call");

    auto rejectedEnd = adapter->end(env.js);
    KJ_ASSERT(rejectedEnd.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "Second end() call should be rejected");

    auto rejectedWrite = adapter->write(env.js, env.js.str("data"_kj));
    KJ_ASSERT(rejectedWrite.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "Write after end() call should be rejected");

    auto rejectedFlush = adapter->flush(env.js);
    KJ_ASSERT(rejectedFlush.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "Flush after end() call should be rejected");

    return env.context
        .awaitJs(env.js, endPromise.then(env.js, [&adapter = *adapter](jsg::Lock& js) {
      KJ_ASSERT(
          adapter.isClosed() == true, "Adapter should be closed after end() promise resolves");
      KJ_ASSERT(adapter.isClosing() == false,
          "Adapter should not be in closing state after end() promise resolves");
      KJ_ASSERT(
          adapter.isErrored() == kj::none, "Adapter should not be errored after successful end()");
      KJ_ASSERT(adapter.getDesiredSize() == kj::none,
          "Desired size should be none after adapter is closed");

      auto rejectedEnd = adapter.end(js);
      KJ_ASSERT(rejectedEnd.getState(js) == jsg::Promise<void>::State::FULFILLED,
          "Second end() call should be fulfilled");

      auto rejectedWrite = adapter.write(js, js.str("data"_kj));
      KJ_ASSERT(rejectedWrite.getState(js) == jsg::Promise<void>::State::REJECTED,
          "Write after end() call should be rejected");

      auto rejectedFlush = adapter.flush(js);
      KJ_ASSERT(rejectedFlush.getState(js) == jsg::Promise<void>::State::REJECTED,
          "Flush after end() call should be rejected");
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Basic abort() operation") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullOutputStream = newNullOutputStream();
    auto sink = newSystemStream(kj::mv(nullOutputStream), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(sink));

    adapter->abort(env.js, env.js.str("Abort reason"_kj));

    KJ_ASSERT(adapter->isClosed() == false, "Adapter should not be closed after abort()");
    KJ_ASSERT(adapter->isClosing() == false, "Adapter should not be closing after abort()");
    auto exception =
        KJ_ASSERT_NONNULL(adapter->isErrored(), "Adapter should be in errored state after abort()");
    KJ_ASSERT(exception.getDescription().contains("Abort reason"),
        "Adapter should be in errored state after abort()");

    KJ_ASSERT(adapter->getDesiredSize() == kj::none,
        "Desired size should be none after adapter is errored");

    auto rejectedWrite = adapter->write(env.js, env.js.str("data"_kj));
    KJ_ASSERT(rejectedWrite.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "Write after abort() call should be rejected");

    auto rejectedFlush = adapter->flush(env.js);
    KJ_ASSERT(rejectedFlush.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "Flush after abort() call should be rejected");

    auto rejectedEnd = adapter->end(env.js);
    KJ_ASSERT(rejectedEnd.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "End after abort() call should be rejected");

    adapter->abort(env.js, env.js.str("Abort reason 2"_kj));
    auto exception2 = KJ_ASSERT_NONNULL(
        adapter->isErrored(), "Adapter should still be in errored state after second abort()");
    KJ_ASSERT(exception2.getDescription().contains("Abort reason 2"),
        "Adapter should reflect reason from second abort() call");
  });
}

KJ_TEST("Abort from closing state supersedes close") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullOutputStream = newNullOutputStream();
    auto sink = newSystemStream(kj::mv(nullOutputStream), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(sink));

    auto endPromise = adapter->end(env.js);
    KJ_ASSERT(endPromise.getState(env.js) == jsg::Promise<void>::State::PENDING,
        "End promise should be pending immediately after end() call");
    KJ_ASSERT(adapter->isClosing() == true,
        "Adapter should be in closing state immediately after end() call");

    adapter->abort(env.js, env.js.str("Abort reason"_kj));

    KJ_ASSERT(adapter->isClosed() == false, "Adapter should not be closed after abort()");
    KJ_ASSERT(adapter->isClosing() == false, "Adapter should not be closing after abort()");
    auto exception =
        KJ_ASSERT_NONNULL(adapter->isErrored(), "Adapter should be in errored state after abort()");
    KJ_ASSERT(exception.getDescription().contains("Abort reason"),
        "Adapter should be in errored state after abort()");

    KJ_ASSERT(adapter->getDesiredSize() == kj::none,
        "Desired size should be none after adapter is errored");

    auto rejectedWrite = adapter->write(env.js, env.js.str("data"_kj));
    KJ_ASSERT(rejectedWrite.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "Write after abort() call should be rejected");

    auto rejectedFlush = adapter->flush(env.js);
    KJ_ASSERT(rejectedFlush.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "Flush after abort() call should be rejected");

    auto rejectedEnd = adapter->end(env.js);
    KJ_ASSERT(rejectedEnd.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "End after abort() call should be rejected");

    return env.context
        .awaitJs(env.js, endPromise.then(env.js, [](jsg::Lock& js) {
      return js.rejectedPromise<void>(js.error("End promise should not resolve after abort()"));
    }, [](jsg::Lock& js, jsg::Value exception) {
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Abort from closed state") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullOutputStream = newNullOutputStream();
    auto sink = newSystemStream(kj::mv(nullOutputStream), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(sink));

    auto endPromise = adapter->end(env.js);
    return env.context.awaitJs(env.js, kj::mv(endPromise))
        .then([adapter = kj::mv(adapter)]() mutable {
      KJ_ASSERT(adapter->isClosed() == true, "Adapter should be closed after end()");
      adapter->abort(KJ_EXCEPTION(FAILED, "Abort after closed should be no-op"));
      KJ_ASSERT(adapter->isClosed() == false,
          "Adapter switches to errored state after abort() from closed state");
      KJ_ASSERT_NONNULL(adapter->isErrored(),
          "Adapter should be in errored state after abort() from closed state");
    });
  });
}

KJ_TEST("Abort rejects ready promise with abort reason") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullOutputStream = newNullOutputStream();
    auto sink = newSystemStream(kj::mv(nullOutputStream), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(sink),
        WritableStreamSinkJsAdapter::Options{
          .highWaterMark = 1,
        });
    adapter->write(env.js, env.js.str("data"_kj));
    adapter->write(env.js, env.js.str("data2"_kj));

    auto readyPromise = adapter->getReady(env.js);
    KJ_ASSERT(readyPromise.getState(env.js) == jsg::Promise<void>::State::PENDING,
        "Teady promise should be pending");

    adapter->abort(env.js, env.js.str("Abort reason"_kj));

    return env.context
        .awaitJs(env.js, readyPromise.then(env.js, [](jsg::Lock& js) {
      return js.rejectedPromise<void>(js.error("Ready promise should not resolve after abort()"));
    }, [](jsg::Lock& js, jsg::Value exception) {
      auto ex = jsg::JsValue(exception.getHandle(js));
      KJ_ASSERT(ex.toString(js).contains("Abort reason"),
          "Ready promise should be rejected with abort reason");
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Abort aborts underlying sink") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    SimpleEventRecordingSink sink;
    kj::Own<SimpleEventRecordingSink> fake(&sink, kj::NullDisposer::instance);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(fake));
    adapter->abort(env.js, env.js.str("Abort reason"_kj));
    KJ_ASSERT(
        sink.getState().abortCalled == true, "Underlying sink's abort() should have been called");
  });
}

KJ_TEST("Abort rejects in-flight operations") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<WritableStreamSink> neverDoneSink = kj::heap<NeverReadySink>();
    auto adapter =
        kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(neverDoneSink));

    auto writePromise = adapter->write(env.js, env.js.str("data"_kj));
    auto flushPromise = adapter->flush(env.js);
    auto endPromise = adapter->end(env.js);

    adapter->abort(env.js, env.js.str("Abort reason"_kj));

    return env.context
        .awaitJs(env.js,
            endPromise.then(env.js,
                [](jsg::Lock& js) {
      return js.rejectedPromise<void>(js.error("End promise should not resolve after abort()"));
    },
                [writePromise = kj::mv(writePromise), flushPromise = kj::mv(flushPromise)](
                    jsg::Lock& js, jsg::Value exception) mutable {
      KJ_ASSERT(writePromise.getState(js) == jsg::Promise<void>::State::REJECTED,
          "Write promise should be rejected after abort()");
      KJ_ASSERT(flushPromise.getState(js) == jsg::Promise<void>::State::REJECTED,
          "Flush promise should be rejected after abort()");

      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("end() waits for all pending writes to complete") {
  TestFixture fixture;
  SimpleEventRecordingSink sink;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<SimpleEventRecordingSink> fake(&sink, kj::NullDisposer::instance);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(fake));

    adapter->write(env.js, env.js.str("data1"_kj));
    adapter->write(env.js, env.js.str("data2"_kj));
    adapter->write(env.js, env.js.str("data3"_kj));
    adapter->write(env.js, env.js.str("data4"_kj));
    KJ_ASSERT(sink.getState().writeCalled == 1,
        "Underlying sink's write() should have been called four times");

    auto endPromise = adapter->end(env.js);

    return env.context
        .awaitJs(env.js, endPromise.then(env.js, [&state = sink.getState()](jsg::Lock& js) {
      KJ_ASSERT(state.writeCalled == 4,
          "Underlying sink's write() should have been called four times before end() resolves");
      KJ_ASSERT(state.endCalled == true, "Underlying sink's end() should have been called");
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("end() waits for all pending flushes to complete") {
  TestFixture fixture;
  SimpleEventRecordingSink sink;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<SimpleEventRecordingSink> fake(&sink, kj::NullDisposer::instance);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(fake));

    auto flush1 = adapter->flush(env.js);
    auto flush2 = adapter->flush(env.js);

    auto endPromise = adapter->end(env.js);

    return env.context
        .awaitJs(env.js,
            endPromise.then(env.js,
                [&state = sink.getState(), flush1 = kj::mv(flush1), flush2 = kj::mv(flush2)](
                    jsg::Lock& js) mutable {
      KJ_ASSERT(state.endCalled == true, "Underlying sink's end() should have been called");
      KJ_ASSERT(flush1.getState(js) == jsg::Promise<void>::State::FULFILLED,
          "First flush() promise should be fulfilled before end() resolves");
      KJ_ASSERT(flush2.getState(js) == jsg::Promise<void>::State::FULFILLED,
          "Second flush() promise should be fulfilled before end() resolves");
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("end() with large queue of pending operations") {
  TestFixture fixture;
  SimpleEventRecordingSink sink;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<SimpleEventRecordingSink> fake(&sink, kj::NullDisposer::instance);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(fake));

    for (int i = 0; i < 1024; i++) {
      adapter->write(env.js, env.js.str("data"_kj));
      adapter->flush(env.js);
    }

    auto endPromise = adapter->end(env.js);

    return env.context
        .awaitJs(env.js, endPromise.then(env.js, [&state = sink.getState()](jsg::Lock& js) {
      KJ_ASSERT(state.writeCalled == 1024,
          "Underlying sink's write() should have been called four times before end() resolves");
      KJ_ASSERT(state.endCalled == true, "Underlying sink's end() should have been called");
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("end() when underlyink sink.end() fails should error adapter") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto throwingSink = kj::heap<ThrowingSink>();
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(throwingSink));

    auto endPromise = adapter->end(env.js);

    return env.context
        .awaitJs(env.js, endPromise.then(env.js, [](jsg::Lock& js) {
      return js.rejectedPromise<void>(js.error("End promise should not resolve when sink fails"));
    }, [&adapter = *adapter](jsg::Lock& js, jsg::Value exception) {
      auto err = jsg::JsValue(exception.getHandle(js));
      KJ_ASSERT(err.toString(js).contains("internal error"));
      KJ_ASSERT(adapter.isErrored() != kj::none, "Adapter should be in errored state");
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("flush() completes after all prior writes") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto recordingSink = kj::heap<SimpleEventRecordingSink>();
    auto& state = recordingSink->getState();
    auto adapter =
        kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(recordingSink));

    adapter->write(env.js, env.js.str("data1"_kj));
    adapter->write(env.js, env.js.str("data2"_kj));
    KJ_ASSERT(state.writeCalled == 1, "Underlying sink's write() should have been called twice");

    auto flushPromise = adapter->flush(env.js);

    return env.context
        .awaitJs(env.js, flushPromise.then(env.js, [&state](jsg::Lock& js) {
      KJ_ASSERT(state.writeCalled == 2,
          "Underlying sink's write() should have been called twice before flush() resolves");
      return js.resolvedPromise();
    })).attach(kj::mv(state), kj::mv(adapter));
  });
}

KJ_TEST("flush() with no writes completes immediately") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto recordingSink = kj::heap<SimpleEventRecordingSink>();
    auto adapter =
        kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(recordingSink));

    auto flushPromise = adapter->flush(env.js);

    return env.context.awaitJs(env.js, kj::mv(flushPromise)).attach(kj::mv(adapter));
  });
}

KJ_TEST("multiple sequential flush() calls") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto recordingSink = kj::heap<SimpleEventRecordingSink>();
    auto adapter =
        kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(recordingSink));

    auto flush1 = adapter->flush(env.js);
    auto flush2 = adapter->flush(env.js);

    return env.context
        .awaitJs(env.js, flush1.then(env.js, [flush2 = kj::mv(flush2)](jsg::Lock& js) mutable {
      return kj::mv(flush2);
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("write() when underlyink sink.write() fails should error adapter") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto throwingSink = kj::heap<ThrowingSink>();
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(throwingSink));

    auto writePromise = adapter->write(env.js, env.js.str("data"_kj));
    auto flushPromise = adapter->flush(env.js);

    return env.context
        .awaitJs(env.js,
            writePromise.then(env.js,
                [](jsg::Lock& js) {
      return js.rejectedPromise<void>(js.error("Write promise should not resolve when sink fails"));
    },
                [&adapter = *adapter, flushPromise = kj::mv(flushPromise)](
                    jsg::Lock& js, jsg::Value exception) mutable {
      auto err = jsg::JsValue(exception.getHandle(js));
      KJ_ASSERT(err.toString(js).contains("internal error"));
      KJ_ASSERT(adapter.isErrored() != kj::none, "Adapter should be in errored state");
      return flushPromise.then(js, [](jsg::Lock& js) {
        return js.rejectedPromise<void>(
            js.error("Flush promise should not resolve when sink fails"));
      }, [](jsg::Lock& js, jsg::Value exception) { return js.resolvedPromise(); });
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("multiple writes() should only error adapter once") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto throwingSink = kj::heap<ThrowingSink>();
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(throwingSink));

    auto write1 = adapter->write(env.js, env.js.str("data"_kj));
    auto write2 = adapter->write(env.js, env.js.str("data"_kj));

    return env.context
        .awaitJs(env.js, write1.then(env.js, [](jsg::Lock& js) {
      return js.rejectedPromise<void>(js.error("Write promise should not resolve when sink fails"));
    }, [&adapter = *adapter, write2 = kj::mv(write2)](jsg::Lock& js, jsg::Value exception) mutable {
      auto err = jsg::JsValue(exception.getHandle(js));
      KJ_ASSERT(err.toString(js).contains("internal error"));
      KJ_ASSERT(adapter.isErrored() != kj::none, "Adapter should be in errored state");
      return write2.then(js, [](jsg::Lock& js) {
        return js.rejectedPromise<void>(
            js.error("Write promise should not resolve when sink fails"));
      }, [](jsg::Lock& js, jsg::Value exception) { return js.resolvedPromise(); });
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("zero-length writes are a non-op (string)") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto recordingSink = kj::heap<SimpleEventRecordingSink>();
    auto& state = recordingSink->getState();
    auto adapter =
        kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(recordingSink));

    auto writePromise = adapter->write(env.js, env.js.str(""_kj));
    KJ_ASSERT(state.writeCalled == 0, "Underlying sink's write() should not have been called");

    return env.context
        .awaitJs(env.js, writePromise.then(env.js, [&state](jsg::Lock& js) {
      KJ_ASSERT(state.writeCalled == 0, "Underlying sink's write() should not have been called");
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("zero-length writes are a non-op (ArrayBuffer)") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto recordingSink = kj::heap<SimpleEventRecordingSink>();
    auto& state = recordingSink->getState();
    auto adapter =
        kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(recordingSink));

    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(env.js, 0);
    jsg::BufferSource source(env.js, kj::mv(backing));
    jsg::JsValue handle(source.getHandle(env.js));

    auto writePromise = adapter->write(env.js, handle);
    KJ_ASSERT(state.writeCalled == 0, "Underlying sink's write() should not have been called");

    return env.context
        .awaitJs(env.js, writePromise.then(env.js, [&state](jsg::Lock& js) {
      KJ_ASSERT(state.writeCalled == 0, "Underlying sink's write() should not have been called");
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("writing small ArrayBuffer") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto recordingSink = kj::heap<SimpleEventRecordingSink>();
    auto& state = recordingSink->getState();
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(recordingSink),
        WritableStreamSinkJsAdapter::Options{
          .highWaterMark = 10,
        });

    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(env.js, 10);
    jsg::BufferSource source(env.js, kj::mv(backing));
    jsg::JsValue handle(source.getHandle(env.js));

    auto writePromise = adapter->write(env.js, handle);
    KJ_ASSERT(state.writeCalled == 1, "Underlying sink's write() should not have been called");
    KJ_ASSERT(KJ_ASSERT_NONNULL(adapter->getDesiredSize()) == 0,
        "Adapter's desired size should be 0 after writing highWaterMark bytes");

    return env.context
        .awaitJs(env.js, writePromise.then(env.js, [&state, &adapter = *adapter](jsg::Lock& js) {
      KJ_ASSERT(state.writeCalled == 1, "Underlying sink's write() should not have been called");
      KJ_ASSERT(KJ_ASSERT_NONNULL(adapter.getDesiredSize()) == 10,
          "Back to initial desired size after write completes");
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("writing medium ArrayBuffer") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto recordingSink = kj::heap<SimpleEventRecordingSink>();
    auto& state = recordingSink->getState();
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(recordingSink),
        WritableStreamSinkJsAdapter::Options{
          .highWaterMark = 5 * 1024,
        });

    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(env.js, 4 * 1024);
    jsg::BufferSource source(env.js, kj::mv(backing));
    jsg::JsValue handle(source.getHandle(env.js));

    auto writePromise = adapter->write(env.js, handle);
    KJ_ASSERT(state.writeCalled == 1, "Underlying sink's write() should not have been called");
    KJ_ASSERT(KJ_ASSERT_NONNULL(adapter->getDesiredSize()) == 1024,
        "Adapter's desired size should be 1024 after writing 4 * 1024 bytes");

    return env.context
        .awaitJs(env.js, writePromise.then(env.js, [&state, &adapter = *adapter](jsg::Lock& js) {
      KJ_ASSERT(state.writeCalled == 1, "Underlying sink's write() should not have been called");
      KJ_ASSERT(KJ_ASSERT_NONNULL(adapter.getDesiredSize()) == 5 * 1024,
          "Back to initial desired size after write completes");
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("writing large ArrayBuffer") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto recordingSink = kj::heap<SimpleEventRecordingSink>();
    auto& state = recordingSink->getState();
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(recordingSink),
        WritableStreamSinkJsAdapter::Options{
          .highWaterMark = 8 * 1024,
        });

    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(env.js, 16 * 1024);
    jsg::BufferSource source(env.js, kj::mv(backing));
    jsg::JsValue handle(source.getHandle(env.js));

    auto writePromise = adapter->write(env.js, handle);
    KJ_ASSERT(state.writeCalled == 1, "Underlying sink's write() should not have been called");
    KJ_ASSERT(KJ_ASSERT_NONNULL(adapter->getDesiredSize()) == -(8 * 1024),
        "Adapter's desired size should be negative after writing 16 * 1024 bytes");

    return env.context
        .awaitJs(env.js, writePromise.then(env.js, [&state, &adapter = *adapter](jsg::Lock& js) {
      KJ_ASSERT(state.writeCalled == 1, "Underlying sink's write() should not have been called");
      KJ_ASSERT(KJ_ASSERT_NONNULL(adapter.getDesiredSize()) == 8 * 1024,
          "Back to initial desired size after write completes");
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("writing the wrong types reject") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto recordingSink = kj::heap<SimpleEventRecordingSink>();
    auto adapter =
        kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(recordingSink));

    auto writeNull = adapter->write(env.js, env.js.null());
    KJ_ASSERT(writeNull.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "Write of null should be rejected");

    auto writeUndefined = adapter->write(env.js, env.js.undefined());
    KJ_ASSERT(writeUndefined.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "Write of undefined should be rejected");

    auto writeNumber = adapter->write(env.js, env.js.num(42));
    KJ_ASSERT(writeNumber.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "Write of number should be rejected");

    auto writeBoolean = adapter->write(env.js, env.js.boolean(true));
    KJ_ASSERT(writeBoolean.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "Write of boolean should be rejected");

    auto writeObject = adapter->write(env.js, env.js.obj());
    KJ_ASSERT(writeObject.getState(env.js) == jsg::Promise<void>::State::REJECTED,
        "Write of plain object should be rejected");
  });
}

KJ_TEST("large number of large writes") {
  TestFixture fixture;
  SimpleEventRecordingSink sink;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<SimpleEventRecordingSink> fake(&sink, kj::NullDisposer::instance);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(fake));

    for (int i = 0; i < 1000; i++) {
      auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(env.js, 16 * 1024);
      jsg::BufferSource source(env.js, kj::mv(backing));
      jsg::JsValue handle(source.getHandle(env.js));

      adapter->write(env.js, handle);
    }
    auto endPromise = adapter->end(env.js);

    return env.context
        .awaitJs(env.js,
            endPromise.then(env.js, [&state = sink.getState(), &adapter = *adapter](jsg::Lock& js) {
      KJ_ASSERT(state.writeCalled == 1000, "Underlying sink's write() should have been called");
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("ready promise signals backpressure correctly") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto recordingSink = kj::heap<SimpleEventRecordingSink>();
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(recordingSink),
        WritableStreamSinkJsAdapter::Options{
          .highWaterMark = 10,
        });

    auto readyPromise = adapter->getReady(env.js);
    KJ_ASSERT(readyPromise.getState(env.js) == jsg::Promise<void>::State::FULFILLED,
        "Ready promise should be fulfilled when no backpressure");

    auto writePromise = adapter->write(env.js, env.js.str("12345678909876543210"_kj));

    readyPromise = adapter->getReady(env.js);
    KJ_ASSERT(readyPromise.getState(env.js) == jsg::Promise<void>::State::PENDING,
        "Ready promise should be fulfilled when no backpressure");

    return env.context
        .awaitJs(env.js, writePromise.then(env.js, [&adapter = *adapter](jsg::Lock& js) {
      auto readyPromise = adapter.getReady(js);
      KJ_ASSERT(readyPromise.getState(js) == jsg::Promise<void>::State::FULFILLED,
          "Ready promise should be fulfilled when no backpressure");
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("detachOnWrite option detaches ArrayBuffer before write") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto recordingSink = kj::heap<SimpleEventRecordingSink>();
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(recordingSink),
        WritableStreamSinkJsAdapter::Options{
          .detachOnWrite = true,
        });

    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(env.js, 10);
    jsg::BufferSource source(env.js, kj::mv(backing));
    KJ_ASSERT(!source.isDetached());
    jsg::JsValue handle(source.getHandle(env.js));

    auto writePromise = adapter->write(env.js, handle);

    jsg::BufferSource source2(env.js, handle);
    KJ_ASSERT(source2.size() == 0);

    return env.context.awaitJs(env.js, kj::mv(writePromise)).attach(kj::mv(adapter));
  });
}

KJ_TEST("detachOnWrite option detaches Uint8Array before write") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto recordingSink = kj::heap<SimpleEventRecordingSink>();
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(recordingSink),
        WritableStreamSinkJsAdapter::Options{
          .detachOnWrite = true,
        });

    auto backing = jsg::BackingStore::alloc<v8::Uint8Array>(env.js, 10);
    jsg::BufferSource source(env.js, kj::mv(backing));
    KJ_ASSERT(!source.isDetached());
    jsg::JsValue handle(source.getHandle(env.js));

    auto writePromise = adapter->write(env.js, handle);

    jsg::BufferSource source2(env.js, handle);
    KJ_ASSERT(source2.size() == 0);

    return env.context.awaitJs(env.js, kj::mv(writePromise)).attach(kj::mv(adapter));
  });
}

KJ_TEST("Creating adapter and dropping it with pending operations") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullOutputStream = newNullOutputStream();
    auto sink = newSystemStream(kj::mv(nullOutputStream), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(sink));

    adapter->write(env.js, env.js.str("data"_kj));
    adapter->flush(env.js);
    adapter->end(env.js);

    // Dropping the adapter here should not crash or leak memory.
  });
}

KJ_TEST("Dropping the IoContext with pending operations and using the adapter in another context") {
  TestFixture fixture;
  kj::Maybe<kj::Own<WritableStreamSinkJsAdapter>> adapter;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullOutputStream = newNullOutputStream();
    auto sink = newSystemStream(kj::mv(nullOutputStream), StreamEncoding::IDENTITY, env.context);
    adapter = kj::heap<WritableStreamSinkJsAdapter>(env.js, env.context, kj::mv(sink));
    auto& adapterRef = *KJ_ASSERT_NONNULL(adapter);

    adapterRef.write(env.js, env.js.str("data"_kj));
    adapterRef.flush(env.js);
    adapterRef.end(env.js);

    // Dropping the IoContext here should not crash or leak memory.
  });

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& otherContext = KJ_ASSERT_NONNULL(adapter);
    try {
      otherContext->write(env.js, env.js.str("data2"_kj));
    } catch (...) {
      auto ex = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(ex.getDescription().startsWith(
          "jsg.Error: Cannot perform I/O on behalf of a different request."));
    }
  });
}

// ================================================================================================

namespace {
struct WritableStreamContext {
  kj::Vector<kj::Array<const kj::byte>> chunks;
  bool closed = false;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> maybeAbort;
};

jsg::Ref<WritableStream> createSimpleWritableStream(jsg::Lock& js, WritableStreamContext& context) {
  return WritableStream::constructor(js,
      UnderlyingSink{
        .write =
            [&context](jsg::Lock& js, auto chunk, auto) {
    jsg::BufferSource source(js, chunk);
    auto data = kj::heapArray<kj::byte>(source.asArrayPtr());
    context.chunks.add(kj::mv(data));
    return js.resolvedPromise();
  },
        .abort =
            [&context](jsg::Lock& js, auto reason) {
    context.maybeAbort = jsg::JsRef<jsg::JsValue>(js, jsg::JsValue(reason));
    return js.resolvedPromise();
  },
        .close =
            [&context](jsg::Lock& js) {
    context.closed = true;
    return js.resolvedPromise();
  },
      },
      StreamQueuingStrategy{});
}

jsg::Ref<WritableStream> createErroredStream(jsg::Lock& js) {
  return WritableStream::constructor(js,
      UnderlyingSink{
        .write = [](jsg::Lock& js, auto chunk,
                     auto) { return js.rejectedPromise<void>(js.error("Write error")); },
        .abort = [](jsg::Lock& js, auto reason) { return js.resolvedPromise(); },
        .close = [](jsg::Lock& js) { return js.resolvedPromise(); },
      },
      StreamQueuingStrategy{});
}

struct FiniteReadableStreamSource final: public ReadableStreamSource {
  int counter = 0;
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    if (++counter < 5) {
      kj::ArrayPtr<kj::byte> buf(static_cast<kj::byte*>(buffer), maxBytes);
      buf.fill('a');
      return maxBytes;
    }
    static constexpr size_t eof = 0;
    return eof;
  }
};

struct ErroringStreamSource final: public ReadableStreamSource {
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return KJ_EXCEPTION(FAILED, "worker_do_not_log: Read error");
  }
};
}  // namespace

KJ_TEST("WritableStreamSinkKjAdapter construction") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  WritableStreamContext context;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createSimpleWritableStream(env.js, context);
    auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));
  });
}

KJ_TEST("WritableStreamSinkKjAdapter construction with locked stream") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  WritableStreamContext context;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createSimpleWritableStream(env.js, context);
    auto writer = stream->getWriter(env.js);

    try {
      auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));
      KJ_FAIL_ASSERT("Construction with locked stream should have thrown");
    } catch (...) {
      auto ex = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(ex.getDescription().contains("WritableStream is locked"));
    }
  });
}

KJ_TEST("WritableStreamSinkKjAdapter construction with closed stream") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  WritableStreamContext context;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createSimpleWritableStream(env.js, context);
    stream->close(env.js);

    auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));
  });
}

KJ_TEST("WritableStreamSinkKjAdapter construction with errored stream") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  WritableStreamContext context;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createSimpleWritableStream(env.js, context);
    stream->abort(env.js, env.js.str("Abort reason"_kj));

    auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));
  });
}

KJ_TEST("WritableStreamSinkKjAdapter construction with immediate end") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  WritableStreamContext context;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createSimpleWritableStream(env.js, context);
    auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));
    return adapter->end().attach(kj::mv(adapter));
  });
}

KJ_TEST("WritableStreamSinkKjAdapter construction with immediate abort") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  WritableStreamContext context;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createSimpleWritableStream(env.js, context);
    auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));
    adapter->abort(KJ_EXCEPTION(DISCONNECTED, "Abort reason"));
  });
}

KJ_TEST("WritableStreamSinkKjAdapter single write") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  WritableStreamContext context;
  kj::FixedArray<kj::byte, 1024> buffer;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createSimpleWritableStream(env.js, context);
    auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));

    buffer.fill('a');
    return adapter->write(buffer.asPtr()).then([&adapter = *adapter]() {
      return adapter.end();
    }).attach(kj::mv(adapter));
  });

  KJ_ASSERT(context.chunks.size() == 1, "Underlying stream should have received one chunk");
  KJ_ASSERT(context.chunks[0].size() == 1024, "Underlying stream chunk should be 1024 bytes");
  KJ_ASSERT(context.chunks[0] == buffer, "Underlying stream chunk should match written data");
  KJ_ASSERT(context.closed, "Underlying stream should be closed");
  KJ_ASSERT(context.maybeAbort == kj::none, "Underlying stream should not be aborted");
}

KJ_TEST("WritableStreamSinkKjAdapter zero-length write") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  WritableStreamContext context;
  kj::ArrayPtr<kj::byte> buffer = nullptr;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createSimpleWritableStream(env.js, context);
    auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));

    return adapter->write(buffer).then([&adapter = *adapter]() {
      return adapter.end();
    }).attach(kj::mv(adapter));
  });

  KJ_ASSERT(context.chunks.size() == 0, "Underlying stream should not have chunks");
  KJ_ASSERT(context.closed, "Underlying stream should be closed");
  KJ_ASSERT(context.maybeAbort == kj::none, "Underlying stream should not be aborted");
}

KJ_TEST("WritableStreamSinkKjAdapter concurrent writes forbidden") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  WritableStreamContext context;
  kj::FixedArray<kj::byte, 100> buffer;

  try {
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      auto stream = createSimpleWritableStream(env.js, context);
      auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));

      buffer.asPtr().fill('a');

      auto p1 = adapter->write(buffer.asPtr());
      // The second one should fail.

      return adapter->write(buffer.asPtr()).attach(kj::mv(adapter));
    });
  } catch (...) {
    auto ex = kj::getCaughtExceptionAsKj();
    KJ_ASSERT(ex.getDescription().contains("Cannot have multiple concurrent writes"));
  }
}

KJ_TEST("WritableStreamSinkKjAdapter write after close") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  WritableStreamContext context;
  kj::FixedArray<kj::byte, 100> buffer;

  try {
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      auto stream = createSimpleWritableStream(env.js, context);
      auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));

      buffer.asPtr().fill('a');

      return adapter->end()
          .then([&adapter = *adapter, &buffer]() {
        return adapter.write(buffer.asPtr());
      }).attach(kj::mv(adapter));
    });
  } catch (...) {
    auto ex = kj::getCaughtExceptionAsKj();
    KJ_ASSERT(ex.getDescription().contains("Cannot write after close"));
  }
}

KJ_TEST("WritableStreamSinkKjAdapter single errored") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  kj::FixedArray<kj::byte, 1024> buffer;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createErroredStream(env.js);
    auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));

    buffer.fill('a');
    return adapter->write(buffer.asPtr())
        .then([&adapter = *adapter]() { KJ_FAIL_ASSERT("Write should have failed"); },
            [](kj::Exception exception) {
      KJ_ASSERT(exception.getDescription().contains("Write error"),
          "Write should have failed with underlying stream error");
    }).attach(kj::mv(adapter));
  });
}

KJ_TEST("WritableStreamSinkKjAdapter pump from") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  FiniteReadableStreamSource source;
  WritableStreamContext context;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createSimpleWritableStream(env.js, context);
    auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));

    auto pumped = KJ_ASSERT_NONNULL(adapter->tryPumpFrom(source, true));
    return env.context.waitForDeferredProxy(kj::mv(pumped)).attach(kj::mv(adapter));
  });

  KJ_ASSERT(context.chunks.size() == 4, "Underlying stream should have received four chunks");
  for (auto& chunk: context.chunks) {
    KJ_ASSERT(chunk.size() == 16384, "Underlying stream chunk should be 16384 bytes");
  }
  KJ_ASSERT(context.closed, "Underlying stream should be closed");
}

KJ_TEST("WritableStreamSinkKjAdapter pump from (no end)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  FiniteReadableStreamSource source;
  WritableStreamContext context;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createSimpleWritableStream(env.js, context);
    auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));

    auto pumped = KJ_ASSERT_NONNULL(adapter->tryPumpFrom(source, false));
    return env.context.waitForDeferredProxy(kj::mv(pumped)).attach(kj::mv(adapter));
  });

  KJ_ASSERT(context.chunks.size() == 4, "Underlying stream should have received four chunks");
  for (auto& chunk: context.chunks) {
    KJ_ASSERT(chunk.size() == 16384, "Underlying stream chunk should be 16384 bytes");
  }
  KJ_ASSERT(!context.closed, "Underlying stream should not be closed");
}

KJ_TEST("WritableStreamSinkKjAdapter pump errored source") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  ErroringStreamSource source;
  WritableStreamContext context;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createSimpleWritableStream(env.js, context);
    auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));

    auto pumped = KJ_ASSERT_NONNULL(adapter->tryPumpFrom(source, false));
    return env.context.waitForDeferredProxy(kj::mv(pumped))
        .then([]() { KJ_FAIL_ASSERT("Pump should have failed"); }, [](kj::Exception exception) {
      KJ_ASSERT(exception.getDescription().contains("Read error"),
          "Pump should have failed with underlying source error");
    }).attach(kj::mv(adapter));
  });
}

KJ_TEST("WritableStreamSinkKjAdapter pump from errored dest") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  FiniteReadableStreamSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createErroredStream(env.js);
    auto adapter = kj::heap<WritableStreamSinkKjAdapter>(env.js, env.context, kj::mv(stream));

    auto pumped = KJ_ASSERT_NONNULL(adapter->tryPumpFrom(source, false));
    return env.context.waitForDeferredProxy(kj::mv(pumped))
        .then([]() { KJ_FAIL_ASSERT("Pump should have failed"); }, [](kj::Exception exception) {
      KJ_ASSERT(exception.getDescription().contains("Write error"),
          "Pump should have failed with underlying dest error");
    }).attach(kj::mv(adapter));
  });
}
}  // namespace workerd::api::streams
