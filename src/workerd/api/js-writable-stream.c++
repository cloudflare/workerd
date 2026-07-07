// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/js-writable-stream.h>

#include <kj/debug.h>

namespace workerd::api {

JsWritableStream::JsWritableStream(jsg::Ref<WritableStream> stream)
    : impl(Impl{
        .stream = StreamImpl(kj::mv(stream)),
      }) {}

JsWritableStream::JsWritableStream(jsg::Lock&, jsg::JsRef<jsg::JsObject>) {
  KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
}

JsWritableStream JsWritableStream::create(jsg::Lock& js,
    IoContext& ioContext,
    kj::Own<WritableStreamSink> sink,
    kj::Maybe<kj::Own<ByteStreamObserver>> observer,
    kj::Maybe<uint64_t> maybeHighWaterMark,
    kj::Maybe<jsg::Promise<void>> maybeClosureWaitable) {
  // TODO(streams-ts): Dispatch on the worker's configuration to construct either the legacy
  // C++ WritableStream or a TypeScript-backed stream.
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
        KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
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
        KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
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
        KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
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
      // TODO(streams-ts): Compose from the isLocked query (rejecting with the exact
      // "This WritableStream is currently locked to a writer." text) and the flush hook.
      KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
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
      KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
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
        KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
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
      KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
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
        KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
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
      KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
    }
  }
}

jsg::Ref<WritableStream> JsWritableStream::getUnderlyingForTest(jsg::Lock& js) {
  auto& i = KJ_ASSERT_NONNULL(impl, "getUnderlyingForTest() called on a null JsWritableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<WritableStream>) {
      return stream.addRef();
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
    }
  }
  KJ_UNREACHABLE;
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

void JsReadableWritablePair::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(readable, writable);
}

void JsReadableWritablePair::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  readable.visitForMemoryInfo(tracker);
  writable.visitForMemoryInfo(tracker);
}

}  // namespace workerd::api
