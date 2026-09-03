// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/js-streams-bridge.h>
#include <workerd/api/js-writable-stream.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/autogate.h>

#include <capnp/compat/byte-stream.h>
#include <kj/common.h>
#include <kj/debug.h>

namespace workerd::api {

namespace {

// Recognizes the TypeScript implementation's WritableStream (including subclasses) by the own
// api-symbol brand its constructor stamps on every instance; the writable counterpart of
// isTypeScriptReadableStream(), where the reasoning is spelled out. Runs no JavaScript, answers
// false for proxies, and is recognition rather than authentication: user code can stamp the
// reflection-visible brand on an object of its own, so consumers re-validate against the real
// #-brand -- here, the writer acquisition that JsWritableStream::serialize() performs before it
// writes anything to the wire.
bool isTypeScriptWritableStream(jsg::Lock& js, jsg::JsObject obj) {
  if (v8::Local<v8::Value>(obj)->IsProxy()) {
    return false;
  }
  return obj.has(js, js.symbolInternal("kWritableStreamBrand"), jsg::JsObject::HasOption::OWN);
}

bool getWritableStreamIsLocked(jsg::Lock& js, jsg::JsObject obj) {
  return webstreams::dispatchCall(js, "isWritableStreamLocked", obj).isTrue();
}

bool getWritableStreamIsClosedOrClosing(jsg::Lock& js, jsg::JsObject obj) {
  return webstreams::dispatchCall(js, "isWritableStreamClosedOrClosing", obj).isTrue();
}

// The forcible abort: the TS-side internal abort algorithm (spec WritableStreamAbort
// equivalent), which does not consult the writer lock.
jsg::Promise<void> writableStreamAbort(
    jsg::Lock& js, jsg::JsObject obj, jsg::Optional<jsg::JsValue>& reason) {
  jsg::JsValue result =
      webstreams::dispatchCall(js, "writableStreamAbort", obj, reason.orDefault(js.undefined()));
  // The result must be a promise
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_PROMISE(result));
  return js.toVoidPromise(promise);
}

// The forcible close: the TS-side internal close algorithm (spec WritableStreamClose
// equivalent), likewise lock-blind. Already closed-or-closing resolves (idempotent);
// errored rejects with the stored error -- matching the legacy controller's close().
jsg::Promise<void> writableStreamClose(jsg::Lock& js, jsg::JsObject obj) {
  jsg::JsValue result = webstreams::dispatchCall(js, "writableStreamClose", obj);
  // The result must be a promise
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_PROMISE(result));
  return js.toVoidPromise(promise);
}

// The forcible flush: resolves once every write queued at call time has completed;
// rejects if the stream is closed/closing (TypeError "This WritableStream has been
// closed.") or errors first. flush is a workerd extension (not in the WHATWG spec); the
// TS side implements it as a positional marker in the controller's write queue,
// mirroring the legacy internal controller's Flush write-event.
jsg::Promise<void> writableStreamFlush(jsg::Lock& js, jsg::JsObject obj) {
  jsg::JsValue result = webstreams::dispatchCall(js, "writableStreamFlush", obj);
  // The result must be a promise
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_PROMISE(result));
  return js.toVoidPromise(promise);
}

void setWritableStreamPendingClosure(jsg::Lock& js, jsg::JsObject obj) {
  // The result is undefined/ignored
  auto res KJ_UNUSED = webstreams::dispatchCall(js, "setWritableStreamPendingClosure", obj);
}

// Permanently neutralizes the stream. Throws (propagated as-is) with the legacy arm's
// exact error behavior: locked / closed-or-closing TypeErrors, or the stored error if
// the stream is errored.
void detachWritableStream(jsg::Lock& js, jsg::JsObject obj) {
  auto res KJ_UNUSED = webstreams::dispatchCall(js, "detachWritableStream", obj);
}

// Convert a write-hook chunk to owned bytes, mirroring the legacy internal controller's
// processChunk (internal.c++): ArrayBuffer / SharedArrayBuffer / ArrayBufferView chunks
// are copied under the isolate lock (the source buffer may be detached, transferred, or
// resized while the write's I/O is in flight), and strings are encoded as UTF-8 (the
// same ergonomic extension the legacy controller applies). Returns kj::none for absent
// or empty chunks -- nothing to write. Throws the legacy byte-types TypeError for
// anything else (on the TS path this rejects the write hook's promise, which errors the
// stream -- a deliberate, flag-guarded delta from the legacy controller's sync throw
// that leaves the stream writable).
kj::Maybe<kj::Array<kj::byte>> chunkToBytes(jsg::Lock& js, jsg::Optional<jsg::JsValue>& value) {
  KJ_IF_SOME(chunk, value) {
    KJ_IF_SOME(ab, chunk.tryCast<jsg::JsArrayBuffer>()) {
      if (ab.size() > 0) return ab.copy();
      return kj::none;
    } else KJ_IF_SOME(sab, chunk.tryCast<jsg::JsSharedArrayBuffer>()) {
      if (sab.size() > 0) return sab.copy();
      return kj::none;
    } else KJ_IF_SOME(view, chunk.tryCast<jsg::JsArrayBufferView>()) {
      if (view.size() > 0) return jsg::JsBufferSource(view).copy();
      return kj::none;
    } else if (chunk.isString()) {
      auto str = chunk.toString(js);
      if (str.size() == 0) return kj::none;
      return webstreams::stringToBytes(kj::mv(str));
    }
    JSG_FAIL_REQUIRE(TypeError, "This WritableStream only supports writing byte types.");
  }
  return kj::none;
}

// The endpoints of a native+native pipe (WritableStreamNativeSink::pipeFrom), held
// refcounted because the failure path needs them after the pump promise rejects, and an
// abort-signal canceler may destroy the pump at any suspension point.
struct PipeFromState {
  kj::Own<WritableStreamSink> sink;
  kj::Own<ReadableStreamSource> source;

  PipeFromState(kj::Own<WritableStreamSink> sink, kj::Own<ReadableStreamSource> source)
      : sink(kj::mv(sink)),
        source(kj::mv(source)) {}
};

// The pipe's pump. A free-standing coroutine (NOT a capturing lambda) so the state
// reference lives in the coroutine frame across suspension points. This pipe is
// isolate-consumed (the returned promise is awaited by JS), so there is no deferred-proxy
// phase to split off; both pump phases simply run to completion.
kj::Promise<void> pipeFromPump(kj::Rc<PipeFromState> state, EndStream end) {
  auto proxy = co_await state->source->pumpTo(state->sink->getPtr(), end == EndStream::YES);
  co_await kj::mv(proxy.proxyTask);
}

// A WritableStreamSink view of a TypeScript-implemented WritableStream, backing
// JsWritableStream::serialize()'s TypeScript arm: it holds the stream's writer (acquired by
// serialize(), which locks the stream) and dispatches each KJ-side operation into the isolate
// to drive it. This is the counterpart of the legacy WritableStreamJsRpcAdapter
// (streams/writable.c++), expressed as a WritableStreamSink so the WritableStreamRpcAdapter
// revoke machinery is reused through newWritableStreamRpcAdapter(): the adapter's revoke path
// cancels any in-flight operation and drops this sink, and the destructor then schedules the
// writer's abort algorithm (matching the legacy adapter's disconnect semantics). Writes are
// delivered to the stream as ArrayBuffers, like the legacy adapter's.
class TsWriterSink final: public WritableStreamSink {
 public:
  TsWriterSink(IoContext& context, jsg::JsRef<jsg::JsObject> writer)
      : context(context),
        writer(kj::mv(writer)) {}

  ~TsWriterSink() noexcept(false) {
    // If the stream was not explicitly ended and the writer is still held, being dropped
    // (the peer released the stream, or the revoke path canceled it) aborts the writer, so
    // the app's abort algorithm runs. The abort is scheduled as a task -- the destructor
    // cannot enter the isolate synchronously -- and may not run at all if the IoContext is
    // already tearing down; both behaviors match the legacy WritableStreamJsRpcAdapter, as
    // does the inability to convey the peer's actual abort reason.
    if (!ended) {
      KJ_IF_SOME(w, writer) {
        scheduleAbort(kj::mv(w), disconnectedException());
      }
      writer = kj::none;
    }
  }

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    if (writer == kj::none) {
      return KJ_EXCEPTION(FAILED, "Write after stream has been closed.");
    }
    if (buffer == nullptr) return kj::READY_NOW;
    return context.run([this, buffer](Worker::Lock& lock) mutable {
      jsg::Lock& js = lock;
      auto ab = jsg::JsArrayBuffer::create(js, buffer);
      return context.awaitJs(
          lock, invokeWriter(js, "writableStreamWriterWrite"_kj, jsg::JsValue(ab)));
    });
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    if (writer == kj::none) {
      return KJ_EXCEPTION(FAILED, "Write after stream has been closed.");
    }
    size_t amount = 0;
    for (auto& piece: pieces) {
      amount += piece.size();
    }
    if (amount == 0) return kj::READY_NOW;
    return context.run([this, amount, pieces](Worker::Lock& lock) mutable {
      jsg::Lock& js = lock;
      // The received buffers are only guaranteed to live until the returned promise
      // resolves, but the application may hold the delivered ArrayBuffer longer, so the
      // bytes must be copied into a fresh allocation.
      auto ab = jsg::JsArrayBuffer::create(js, amount);
      auto ptr = ab.asArrayPtr();
      for (auto& piece: pieces) {
        if (piece.size() == 0) continue;
        ptr.write(piece);
      }
      return context.awaitJs(
          lock, invokeWriter(js, "writableStreamWriterWrite"_kj, jsg::JsValue(ab)));
    });
  }

  kj::Promise<void> end() override {
    if (writer == kj::none) {
      return KJ_EXCEPTION(FAILED, "End after stream has been closed.");
    }
    ended = true;
    return context.run([this](Worker::Lock& lock) mutable {
      jsg::Lock& js = lock;
      return context.awaitJs(lock, invokeWriter(js, "writableStreamWriterClose"_kj));
    });
  }

  void abort(kj::Exception reason) override {
    // Nothing calls this in the RPC wiring (the adapter's revoke path drops the sink and the
    // destructor handles the abort), but the interface requires it: forward the reason to
    // the writer's abort algorithm.
    KJ_IF_SOME(w, writer) {
      scheduleAbort(kj::mv(w), kj::mv(reason));
    }
    writer = kj::none;
  }

 private:
  IoContext& context;
  kj::Maybe<jsg::JsRef<jsg::JsObject>> writer;
  bool ended = false;

  // Drive a writer operation through the frozen cppExports internals (never the public
  // writer prototype methods, which are user-patchable: a replaced write/close must not be
  // able to intercept or fake a stream's RPC transfer), returning its (required) promise
  // result.
  jsg::Promise<void> invokeWriter(
      jsg::Lock& js, kj::StringPtr op, kj::Maybe<jsg::JsValue> arg = kj::none) {
    auto& w = KJ_UNWRAP_OR(writer, { kj::throwFatalException(disconnectedException()); });
    auto result = webstreams::dispatchCall(
        js, op, jsg::JsValue(w.getHandle(js)), arg.orDefault(jsg::JsValue(js.undefined())));
    return js.toVoidPromise(KJ_REQUIRE_NONNULL(
        JSG_TRY_CAST_PROMISE(result), "writer operation did not return a promise", op));
  }

  void scheduleAbort(jsg::JsRef<jsg::JsObject> writer, kj::Exception reason) {
    // Once the last IncomingRequest is gone the IoContext can no longer usefully run JavaScript:
    // the task would be queued onto a task set that is already being torn down, so the app's
    // abort algorithm would never observe it. Drop the writer rather than queue unrunnable work.
    if (!context.hasCurrentIncomingRequest()) return;
    context.addTask(
        context.run([writer = kj::mv(writer), reason = kj::mv(reason)](Worker::Lock& lock) mutable {
      jsg::Lock& js = lock;
      auto ex = js.exceptionToJsValue(kj::mv(reason));
      // Same internal-dispatch requirement as invokeWriter above.
      auto result = webstreams::dispatchCall(js, "writableStreamWriterAbort",
          jsg::JsValue(writer.getHandle(js)), jsg::JsValue(ex.getHandle(js)));
      auto promise = js.toVoidPromise(KJ_REQUIRE_NONNULL(
          JSG_TRY_CAST_PROMISE(result), "writableStreamWriterAbort did not return a promise"));
      return IoContext::current().awaitJs(lock, kj::mv(promise));
    }));
  }

  static kj::Exception disconnectedException() {
    return JSG_KJ_EXCEPTION(DISCONNECTED, Error,
        "WritableStream received over RPC was disconnected because the remote execution context "
        "has ended.");
  }
};

}  // namespace

JsWritableStream::JsWritableStream(jsg::Ref<WritableStream> stream)
    : impl(Impl{
        .stream = StreamImpl(kj::mv(stream)),
      }) {}

JsWritableStream::JsWritableStream(jsg::Lock&, jsg::JsRef<jsg::JsObject> obj)
    : impl(Impl{.stream = kj::mv(obj)}) {}

JsWritableStream JsWritableStream::create(jsg::Lock& js,
    IoContext& ioContext,
    kj::Own<WritableStreamSink> sink,
    kj::Maybe<kj::Own<ByteStreamObserver>> observer,
    kj::Maybe<uint64_t> maybeHighWaterMark,
    kj::Maybe<jsg::Promise<void>> maybeClosureWaitable) {
  if (FeatureFlags::get(js).getTypeScriptImplementedStreams()) {
    // TypeScript-implemented streams: wrap the native sink in a WritableStreamNativeSink
    // -- whose instances are born carrying the kNativeSink marker (JSG_PRIVATE_SYMBOL)
    // that the TypeScript WritableStream constructor detects -- and construct the
    // TypeScript stream over it via the createNativeWritableStream factory exposed
    // through the bootstrap's cpp_exports module. The factory (rather than the raw
    // constructor) is used so the backpressure strategy policy (byte-based sizing when a
    // highWaterMark is configured) lives with the TypeScript implementation.
    auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<WritableStreamNativeSink>>());
    auto sinkObj = jsg::JsValue(handler.wrap(js,
        js.alloc<WritableStreamNativeSink>(
            ioContext, kj::mv(sink), kj::mv(observer), kj::mv(maybeClosureWaitable))));
    jsg::JsValue stream = [&]() -> jsg::JsValue {
      KJ_IF_SOME(highWaterMark, maybeHighWaterMark) {
        return webstreams::dispatchCall(
            js, "createNativeWritableStream", sinkObj, js.num(static_cast<double>(highWaterMark)));
      }
      return webstreams::dispatchCall(js, "createNativeWritableStream", sinkObj);
    }();
    auto obj = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_OBJECT(stream));
    return JsWritableStream(js, obj.addRef(js));
  }
  return JsWritableStream(js.alloc<WritableStream>(
      ioContext, kj::mv(sink), kj::mv(observer), maybeHighWaterMark, kj::mv(maybeClosureWaitable)));
}

JsWritableStream JsWritableStream::addRef(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
        return JsWritableStream(Impl{
          .stream = StreamImpl(stream.addRef()),
        });
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return JsWritableStream(Impl{
          .stream = StreamImpl(obj.addRef(js)),
        });
      }
    }
    KJ_UNREACHABLE;
  }
  // addRef() of a null stream is a null stream.
  return JsWritableStream();
}

bool JsWritableStream::isNull() const {
  return impl == kj::none;
}

bool JsWritableStream::isLocked(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
        return stream->isLocked();
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        if (js.isJavascriptExecutionDisallowed()) {
          // Asking the TypeScript side would execute JS, which is forbidden here. TS-backed
          // streams reachable in a no-JS scope are hydration-fresh (see the fuller reasoning
          // at JsReadableStream::isDisturbed()); fresh streams are unlocked by construction.
          return false;
        }
        return getWritableStreamIsLocked(js, obj.getHandle(js));
      }
    }
    KJ_UNREACHABLE;
  }
  return false;
}

bool JsWritableStream::isClosedOrClosing(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
        return stream->getController().isClosedOrClosing();
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        // NOTE: deliberate (flag-guarded) delta from the legacy arm's heuristic: a pending
        // flush does not count as "closing" on the TS path (the legacy internal controller's
        // queue.back().isCloseOrFlush() quirk treats it as such). See the hook's doc comment
        // in writable.ts.
        return getWritableStreamIsClosedOrClosing(js, obj.getHandle(js));
      }
    }
    KJ_UNREACHABLE;
  }
  return false;
}

jsg::Promise<void> JsWritableStream::flush(jsg::Lock& js) {
  auto& i = KJ_ASSERT_NONNULL(impl, "flush() called on a null JsWritableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
      // WritableStream::flush() itself performs the writer-lock check, rejecting with the
      // exact user-visible TypeError text when locked.
      return stream->flush(js);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      // Composed lock-checked flush: the isLocked query plus the forcible flush hook.
      // Semantically identical to the C++ arm's check-then-act (single-threaded under the
      // isolate lock; no interleaving hazard). The rejection text is user-visible and must
      // match WritableStream::flush() exactly.
      auto handle = obj.getHandle(js);
      if (getWritableStreamIsLocked(js, handle)) {
        return js.rejectedPromise<void>(
            js.typeError("This WritableStream is currently locked to a writer."_kj));
      }
      return writableStreamFlush(js, handle);
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<void> JsWritableStream::forceFlush(jsg::Lock& js) {
  auto& i = KJ_ASSERT_NONNULL(impl, "forceFlush() called on a null JsWritableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
      // Going through the controller (rather than WritableStream::flush()) deliberately
      // bypasses the "is locked to a writer" check.
      return stream->getController().flush(js);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      return writableStreamFlush(js, obj.getHandle(js));
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<void> JsWritableStream::forceAbort(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
        // Going through the controller (rather than WritableStream::abort()) deliberately
        // bypasses the "is locked to a writer" check: this aborts the stream out from under any
        // writer.
        return stream->getController().abort(js, kj::mv(reason));
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        // The TS-side internal abort algorithm is likewise lock-blind.
        return writableStreamAbort(js, obj.getHandle(js), reason);
      }
    }
    KJ_UNREACHABLE;
  }
  // Force-aborting a null stream is a no-op.
  return js.resolvedPromise();
}

jsg::Promise<void> JsWritableStream::forceClose(jsg::Lock& js) {
  auto& i = KJ_ASSERT_NONNULL(impl, "forceClose() called on a null JsWritableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
      // Going through the controller (rather than WritableStream::close()) deliberately
      // bypasses the "is locked to a writer" check.
      return stream->getController().close(js);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      // The TS-side internal close algorithm is likewise lock-blind.
      return writableStreamClose(js, obj.getHandle(js));
    }
  }
  KJ_UNREACHABLE;
}

void JsWritableStream::setPendingClosure(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
        stream->getController().setPendingClosure();
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        setWritableStreamPendingClosure(js, obj.getHandle(js));
      }
    }
  }
}

void JsWritableStream::detach(jsg::Lock& js) {
  auto& i = KJ_ASSERT_NONNULL(impl, "detach() called on a null JsWritableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
      stream->detach(js);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      // The hook throws (locked / closed / stored error) exactly as the legacy arm does;
      // the exception propagates as-is.
      detachWritableStream(js, obj.getHandle(js));
    }
  }
}

jsg::Promise<void> JsWritableStream::writeForTest(jsg::Lock& js, jsg::JsValue chunk) {
  auto& i = KJ_ASSERT_NONNULL(impl, "writeForTest() called on a null JsWritableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
      return stream->getController().write(js, chunk);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      // Drive the write through the ordinary writer machinery: acquire, write, release.
      // Releasing with the write still in flight is fine -- in-flight writes proceed to
      // the sink regardless (only the writer's own promises detach).
      auto handle = obj.getHandle(js);
      auto writer = KJ_REQUIRE_NONNULL(
          JSG_TRY_CAST_OBJECT(webstreams::invokeMethod(js, handle, "getWriter"_kj)),
          "getWriter() did not return an object");
      auto writeResult = webstreams::invokeMethod(js, writer, "write"_kj, chunk);
      auto promise = js.toPromise(v8::Local<v8::Value>(KJ_REQUIRE_NONNULL(
          JSG_TRY_CAST_PROMISE(writeResult), "write() did not return a promise")));
      webstreams::invokeMethod(js, writer, "releaseLock"_kj);
      return promise.then(js, [](jsg::Lock& js, jsg::Value) {});
    }
  }
  KJ_UNREACHABLE;
}

void JsWritableStream::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  auto& i = KJ_ASSERT_NONNULL(impl, "serialize() called on a null JsWritableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
      stream->serialize(js, serializer);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      // Mirrors WritableStream::serialize()'s JavaScript-backed arm (streams/writable.c++):
      // the peer's bytes are driven through the stream's writer, always in IDENTITY encoding.
      // (Like that arm, this could someday learn to extract a wrapped native sink and
      // transfer it encoding-aware; see the detach extension hook.)
      auto& handler = JSG_REQUIRE_NONNULL(serializer.getExternalHandler(), DOMDataCloneError,
          "WritableStream can only be serialized for RPC.");
      auto externalHandler = dynamic_cast<RpcSerializerExternalHandler*>(&handler);
      JSG_REQUIRE(externalHandler != nullptr, DOMDataCloneError,
          "WritableStream can only be serialized for RPC.");

      IoContext& ioctx = IoContext::current();

      // NOTE: We're counting on writer acquisition to check that the stream is not locked
      // and other common checks. It's important we don't modify the WritableStream before
      // this call. Acquisition goes through the frozen cppExports internals -- NOT the
      // public getWriter, which is user-patchable and must not be able to fake the
      // transfer -- with the public method's exact semantics (same constructor path,
      // including the locked TypeError).
      auto writerValue =
          webstreams::dispatchCall(js, "acquireWritableStreamWriter", obj.getHandle(js));
      auto writerObj = KJ_REQUIRE_NONNULL(
          JSG_TRY_CAST_OBJECT(writerValue), "acquireWritableStreamWriter did not return an object");

      auto wrapper =
          newWritableStreamRpcAdapter(kj::heap<TsWriterSink>(ioctx, jsg::JsRef(js, writerObj)));

      // Make sure this stream will be revoked if the IoContext ends.
      ioctx.addTask(wrapper.completionOrRevoke.attach(ioctx.registerPendingEvent()));

      auto capnpStream = ioctx.getByteStreamFactory().kjToCapnp(kj::mv(wrapper.stream));

      externalHandler->write(
          [capnpStream = kj::mv(capnpStream)](rpc::JsValue::External::Builder builder) mutable {
        auto ws = builder.initWritableStream();
        ws.setByteStream(kj::mv(capnpStream));
        ws.setEncoding(StreamEncoding::IDENTITY);
      });
    }
  }
}

void JsWritableStream::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
        visitor.visit(stream);
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        visitor.visit(obj);
      }
    }
  }
}

void JsWritableStream::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
        tracker.trackField("stream", stream);
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        // TODO(streams-ts): track the JS object's memory once TS-backed streams are supported.
        // (jsg::JsRef does not satisfy the MemoryRetainer concept, so it can't be passed to
        // trackField() directly.)
      }
    }
  }
}

kj::Maybe<JsWritableStream> JsWritableStream::tryUnwrapTs(
    jsg::Lock& js, v8::Local<v8::Value> handle) {
  // Without the flag there is no TypeScript implementation (and no bootstrap export to
  // ask), so nothing can match. This also keeps the flag-off unwrap path allocation- and
  // JS-call-free.
  if (!FeatureFlags::get(js).getTypeScriptImplementedStreams()) {
    return kj::none;
  }
  KJ_IF_SOME(obj, JSG_TRY_CAST_OBJECT(jsg::JsValue(handle))) {
    if (isTypeScriptWritableStream(js, obj)) {
      return JsWritableStream(js, obj.addRef(js));
    }
  }
  return kj::none;
}

kj::Maybe<jsg::Ref<WritableStream>> JsWritableStream::tryGetLegacy(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
        return stream.addRef();
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return kj::none;
      }
    }
    KJ_UNREACHABLE;
  }
  // A null stream is backed by neither arm.
  return kj::none;
}

kj::Maybe<jsg::JsObject> JsWritableStream::tryGetTs(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
        return kj::none;
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return obj.getHandle(js);
      }
    }
    KJ_UNREACHABLE;
  }
  // A null stream is backed by neither arm.
  return kj::none;
}

void JsReadableWritablePair::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(readable, writable);
}

void JsReadableWritablePair::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  readable.visitForMemoryInfo(tracker);
  writable.visitForMemoryInfo(tracker);
}

// =======================================================================================
// WritableStreamNativeSink

WritableStreamNativeSink::WritableStreamNativeSink(IoContext& ioContext,
    kj::Own<WritableStreamSink> sink,
    kj::Maybe<kj::Own<ByteStreamObserver>> observer,
    kj::Maybe<jsg::Promise<void>> maybeClosureWaitable)
    : state(Active{.sink = ioContext.addObject(kj::mv(sink))}),
      observer(kj::mv(observer)),
      maybeClosureWaitable(kj::mv(maybeClosureWaitable)) {}

jsg::Promise<void> WritableStreamNativeSink::write(
    jsg::Lock& js, jsg::Optional<jsg::JsValue> chunk) {
  // Defensive: the TS machinery serializes sink operations (at most one write or close in
  // flight), so a concurrent write is a contract violation and must fail loudly.
  JSG_REQUIRE(!writeInFlight, TypeError, "write() is already in flight.");

  KJ_IF_SOME(active, state) {
    KJ_IF_SOME(data, chunkToBytes(js, chunk)) {
      size_t len = data.size();
      auto& ioContext = IoContext::current();

      // Fast path: complete the write synchronously when the sink can accept the bytes
      // immediately, settling the hook's promise without an event-loop round trip.
      // Requires ALL of:
      // - A non-empty chunk.
      // - We are not running in an actor. An actor's output gate must serialize with
      //   writes, and there is no synchronous way to check that the gate is open
      //   (waitForOutputLocksIfNecessary() returns a promise for every actor).
      //
      // The legacy controller's queue-empty condition holds here by construction: the
      // writeInFlight guard above plus the TS machinery's own serialization mean no other
      // sink operation can be outstanding. Backpressure accounting lives in the TS
      // machinery and is driven by this hook promise's settlement, so a synchronously
      // resolved promise is spec-normal.
      if (len > 0 && ioContext.getActor() == kj::none &&
          util::Autogate::isEnabled(util::AutogateKey::STREAM_CONTROLLER_SYNC_FAST_PATHS)) {
        bool syncSuccess = false;
        KJ_TRY {
          syncSuccess = active.sink->tryWriteSync(data.asPtr());
        }
        KJ_CATCH(exception) {
          // tryWriteSync() may throw when a synchronous write is possible but fails.
          // Handle it exactly like the asynchronous catch_ below: the sink is no longer
          // usable; reject the hook's promise so the TS machinery errors the stream.
          state = kj::none;
          return js.rejectedPromise<void>(
              jsg::JsValue(js.exceptionToJs(kj::mv(exception)).getHandle(js)));
        }
        if (syncSuccess) {
          // The write completed synchronously, so there is no in-flight window: no
          // writeInFlight to track and no deferred abort() release to settle. We still
          // inform the observer that a chunk passed through.
          KJ_IF_SOME(o, observer) {
            o->onChunkEnqueued(len);
            o->onChunkDequeued(len);
          }
          return js.resolvedPromise();
        }
      }

      KJ_IF_SOME(o, observer) {
        o->onChunkEnqueued(len);
      }
      writeInFlight = true;
      // Durable Object output gate: the write's bytes must not become externally
      // observable while an output lock is pending, exactly like the legacy internal
      // controller, which stores an output lock with every queued write event and awaits
      // it before touching the sink. The sink reference taken here stays valid for the
      // whole wait+write window: writeInFlight defers any abort()/detach() release to the
      // write's settlement. The write's I/O runs outside the isolate lock; the copied
      // bytes ride the promise.
      kj::Promise<void> promise = nullptr;
      KJ_IF_SOME(lock, ioContext.waitForOutputLocksIfNecessary()) {
        promise = lock.then([&sink = *active.sink, data = kj::mv(data)]() mutable {
          return sink.writeWithBackpressureTracing(data.asPtr()).attach(kj::mv(data));
        });
      } else {
        promise = active.sink->writeWithBackpressureTracing(data.asPtr()).attach(kj::mv(data));
      }
      return ioContext
          .awaitIo(js, kj::mv(promise), [self = JSG_THIS, len](jsg::Lock& js) mutable {
        self->writeInFlight = false;
        KJ_IF_SOME(o, self->observer) {
          o->onChunkDequeued(len);
        }
        if (self->pendingAbort) {
          // abort() arrived while the write was in flight: complete the deferred release.
          self->pendingAbort = false;
          self->state = kj::none;
        }
      }).catch_(js, [self = JSG_THIS, len](jsg::Lock& js, jsg::Value exception) mutable {
        // The write failed; the sink is no longer usable. Rethrow to reject the write
        // hook's promise -- the TS machinery errors the stream.
        self->writeInFlight = false;
        self->pendingAbort = false;
        KJ_IF_SOME(o, self->observer) {
          o->onChunkDequeued(len);
        }
        self->state = kj::none;
        js.throwException(kj::mv(exception));
      });
    }
    // Nothing to write (an absent, undefined, or empty chunk resolves without touching
    // the sink -- matching the legacy controller).
    return js.resolvedPromise();
  }

  // The sink was already released (aborted, closed, or consumed by pipeFrom). The TS
  // machinery stops dispatching once it observes those transitions; tolerate defensively.
  return js.resolvedPromise();
}

jsg::Promise<void> WritableStreamNativeSink::close(jsg::Lock& js) {
  // Defensive: the TS machinery serializes sink operations.
  JSG_REQUIRE(!writeInFlight, TypeError, "close() while a write is in flight.");

  KJ_IF_SOME(waitable, maybeClosureWaitable) {
    // Closing is gated on the waitable (e.g. a Socket's `opened` promise). Consume it:
    // the TS machinery calls close at most once.
    auto promise = kj::mv(waitable);
    maybeClosureWaitable = kj::none;
    return promise.then(js, [self = JSG_THIS](jsg::Lock& js) mutable {
      return self->closeImpl(js);
    }, [self = JSG_THIS](jsg::Lock& js, jsg::Value exception) mutable {
      // Legacy quirk (WritableStreamInternalController::close): a rejected
      // closure waitable resolves the close WITHOUT ending the sink -- the failure is
      // reported through the owning object's own promises (e.g. Socket.closed/opened)
      // instead. Release the sink; it will never be driven again.
      self->state = kj::none;
      return js.resolvedPromise();
    });
  }
  return closeImpl(js);
}

jsg::Promise<void> WritableStreamNativeSink::closeImpl(jsg::Lock& js) {
  KJ_IF_SOME(active, state) {
    auto& ioContext = IoContext::current();
    // Durable Object output gate: like the legacy controller's queued Close event, the
    // sink must not be ended while an output lock is pending. The sink reference stays
    // valid across the wait: the TS machinery serializes sink operations, abort() only
    // releases the sink itself, and pipeFrom() -- the one path that could move the sink
    // out from under this wait -- rejects while closeInFlight is set.
    closeInFlight = true;
    kj::Promise<void> endPromise = nullptr;
    KJ_IF_SOME(lock, ioContext.waitForOutputLocksIfNecessary()) {
      endPromise = lock.then([&sink = *active.sink]() { return sink.end(); });
    } else {
      endPromise = active.sink->end();
    }
    return ioContext
        .awaitIo(js, kj::mv(endPromise), [self = JSG_THIS](jsg::Lock& js) mutable {
      self->closeInFlight = false;
      self->state = kj::none;
    }).catch_(js, [self = JSG_THIS](jsg::Lock& js, jsg::Value exception) mutable {
      // The end failed; the sink is no longer usable either way.
      self->closeInFlight = false;
      self->state = kj::none;
      js.throwException(kj::mv(exception));
    });
  }
  // Already released (an abort raced ahead, or a repeated close); nothing to end.
  return js.resolvedPromise();
}

jsg::Promise<void> WritableStreamNativeSink::abort(
    jsg::Lock& js, jsg::Optional<jsg::JsValue> reason) {
  KJ_IF_SOME(active, state) {
    kj::Exception exception = [&]() {
      KJ_IF_SOME(r, reason) {
        return js.exceptionToKj(r);
      }
      return JSG_KJ_EXCEPTION(DISCONNECTED, Error, "This WritableStream was aborted.");
    }();
    active.sink->abort(kj::mv(exception));
    if (writeInFlight) {
      // A write's I/O is in flight; releasing the sink now would destroy it out from
      // under its own write. Defer the release to the write's settlement.
      pendingAbort = true;
    } else {
      state = kj::none;
    }
  }
  // Aborting an already-released sink is a no-op.
  return js.resolvedPromise();
}

void WritableStreamNativeSink::detach(jsg::Lock& js) {
  if (writeInFlight) {
    // releaseLock() does not wait for pending writes, so a write can genuinely be in
    // flight when detach is called. Defer the sink release to the write's settlement
    // (same mechanism as abort). Queued-but-not-yet-dispatched writes that arrive after
    // detach resolve successfully against the released sink (silent data drop); this
    // matches the legacy controller's detach behavior.
    pendingAbort = true;
  } else {
    state = kj::none;
  }
  // The stream is permanently locked after detach, so close() can never be dispatched
  // again; drop the closure waitable's promise ref eagerly.
  maybeClosureWaitable = kj::none;
}

jsg::Promise<void> WritableStreamNativeSink::pipeFrom(
    jsg::Lock& js, jsg::JsObject sourceObj, jsg::JsObject optionsObj) {
  // The TS pipeTo dispatch extracted both endpoints before calling (extraction only
  // checks the lock), so a released sink here means the stream was already closed or
  // aborted out from under the pipe.
  auto& active = JSG_REQUIRE_NONNULL(state, TypeError, "This WritableStream has been closed.");
  JSG_REQUIRE(!writeInFlight, TypeError, "pipeFrom() while a write is in flight.");
  // The TS pipe dispatch rejects close-queued destinations before extracting, so this is
  // unreachable through pipeTo; it keeps the sink's lifetime preconditions self-contained
  // (the in-flight end() references the sink this call would move into the pump).
  JSG_REQUIRE(!closeInFlight, TypeError, "pipeFrom() while a close() is in flight.");

  // The TS dispatch converted and validated the options BEFORE extraction (spec getter
  // order, spec error text), so these are side-effect-free reads of plain data
  // properties, and the signal check below is defense-in-depth. The dispatch currently
  // routes any pipe with a prevent* option to the JS pump (extraction cannot honor
  // post-pipe endpoint usability), so the prevent* values here are false today; the
  // handling below is kept as the intended fast-path semantics should the dispatch gate
  // ever widen (see the TODO(streams-ts) at the dispatch).
  bool preventAbort = optionsObj.get(js, "preventAbort"_kj).isTrue();
  bool preventCancel = optionsObj.get(js, "preventCancel"_kj).isTrue();
  bool preventClose = optionsObj.get(js, "preventClose"_kj).isTrue();
  auto signalValue = optionsObj.get(js, "signal"_kj);
  kj::Maybe<jsg::Ref<AbortSignal>> maybeSignal;
  if (!signalValue.isUndefined()) {
    auto& signalHandler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<AbortSignal>>());
    maybeSignal = JSG_REQUIRE_NONNULL(signalHandler.tryUnwrap(js, signalValue), TypeError,
        "options.signal must be an AbortSignal");
  }

  // Unwrap the extracted source and release it for the pump (any stashed bytes are folded
  // in ahead of the source by releaseForPump itself).
  auto& sourceHandler =
      KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStreamNativeSource>>());
  auto source = JSG_REQUIRE_NONNULL(sourceHandler.tryUnwrap(js, jsg::JsValue(sourceObj)), TypeError,
      "pipeFrom() requires a native ReadableStream source");
  auto releasedSource = source->releaseForPump(js);

  // Consume the sink: after extraction the TS stream is permanently locked and no further
  // hooks will be dispatched; the pipe owns both endpoints outright.
  auto sink = kj::Own<WritableStreamSink>(kj::mv(active.sink));
  state = kj::none;

  // A pre-aborted signal short-circuits: perform the option-gated shutdown and reject
  // with the signal's reason.
  KJ_IF_SOME(signal, maybeSignal) {
    if (signal->getAborted(js)) {
      auto reason = signal->getReason(js);
      auto exception = js.exceptionToKj(reason);
      if (!preventAbort) sink->abort(exception.clone());
      if (!preventCancel) releasedSource->cancel(kj::mv(exception));
      return js.rejectedPromise<void>(reason);
    }
  }

  auto pipeState = kj::rc<PipeFromState>(kj::mv(sink), kj::mv(releasedSource));
  // Durable Object output gate: like the legacy controller's queued Pipe event, the pump
  // must not start while an output lock is pending. The lock is consulted once, at pipe
  // start, matching the legacy event's single wait. The pump coroutine must be CREATED
  // inside the continuation -- KJ coroutines run eagerly, so a pre-created pump would
  // progress regardless of what its promise is chained behind.
  auto end = preventClose ? EndStream::NO : EndStream::YES;
  kj::Promise<void> pump = nullptr;
  KJ_IF_SOME(lock, IoContext::current().waitForOutputLocksIfNecessary()) {
    pump = lock.then([pipeState = pipeState.addRef(), end]() mutable {
      return pipeFromPump(kj::mv(pipeState), end);
    });
  } else {
    pump = pipeFromPump(pipeState.addRef(), end);
  }
  KJ_IF_SOME(signal, maybeSignal) {
    // The canceler destroys the pump at its suspension point when the signal fires; the
    // shutdown continuation below is attached OUTSIDE the wrap, so it still runs and
    // performs the option-gated teardown with the signal's exception.
    pump = signal->wrap(js, kj::mv(pump));
  }
  pump = pump.catch_([pipeState = kj::mv(pipeState), preventAbort, preventCancel](
                         kj::Exception&& exception) mutable -> kj::Promise<void> {
    // Option-gated shutdown, mirroring the TS pump's error actions: abort the destination
    // unless prevented; cancel the source unless prevented.
    if (!preventAbort) pipeState->sink->abort(exception.clone());
    if (!preventCancel) pipeState->source->cancel(exception.clone());
    return kj::mv(exception);
  });
  auto& ioContext = IoContext::current();
  return ioContext.awaitIo(js, kj::mv(pump), [signal = kj::mv(maybeSignal)](jsg::Lock& js) {
    // The signal ref (if any) rides here only to keep its canceler alive for the pump's
    // duration.
  });
}

void WritableStreamNativeSink::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(maybeClosureWaitable);
}

void WritableStreamNativeSink::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  // The sink and observer are KJ-owned; there is no JS-heap-proportional state to report
  // beyond the object itself.
}

}  // namespace workerd::api
