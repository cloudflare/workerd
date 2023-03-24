// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "writable.h"

namespace workerd::api {

WritableStreamDefaultWriter::WritableStreamDefaultWriter()
    : ioContext(tryGetIoContext()) {}

WritableStreamDefaultWriter::~WritableStreamDefaultWriter() noexcept(false) {
  KJ_IF_MAYBE(stream, state.tryGet<Attached>()) {
    // Because this can be called during gc or other cleanup, it is important
    // that releasing the writer does not cause the closed promise be resolved
    // since that requires v8 heap allocations.
    (*stream)->getController().releaseWriter(*this, nullptr);
  }
}

jsg::Ref<WritableStreamDefaultWriter> WritableStreamDefaultWriter::constructor(
    jsg::Lock& js,
    jsg::Ref<WritableStream> stream) {
  JSG_REQUIRE(!stream->isLocked(), TypeError,
               "This WritableStream is currently locked to a writer.");
  auto writer = jsg::alloc<WritableStreamDefaultWriter>();
  writer->lockToStream(js, *stream);
  return kj::mv(writer);
}

jsg::Promise<void> WritableStreamDefaultWriter::abort(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> reason) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this writer was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      return stream->getController().abort(js, reason);
    }
    KJ_CASE_ONEOF(r, Released) {
      return js.rejectedPromise<void>(
          js.v8TypeError("This WritableStream writer has been released."_kj));
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return js.resolvedPromise();
    }
  }
  KJ_UNREACHABLE;
}

void WritableStreamDefaultWriter::attach(
    WritableStreamController& controller,
    jsg::Promise<void> closedPromise,
    jsg::Promise<void> readyPromise) {
  KJ_ASSERT(state.is<Initial>());
  state = controller.addRef();
  this->closedPromise = kj::mv(closedPromise);
  replaceReadyPromise(kj::mv(readyPromise));
}

jsg::Promise<void> WritableStreamDefaultWriter::close(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this writer was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      return stream->getController().close(js);
    }
    KJ_CASE_ONEOF(r, Released) {
      return js.rejectedPromise<void>(
          js.v8TypeError("This WritableStream writer has been released."_kj));
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return js.rejectedPromise<void>(
          js.v8TypeError("This WritableStream has been closed."_kj));
    }
  }
  KJ_UNREACHABLE;
}

void WritableStreamDefaultWriter::detach() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      // Do nothing in this case.
      return;
    }
    KJ_CASE_ONEOF(stream, Attached) {
      state.init<StreamStates::Closed>();
      return;
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      // Do nothing in this case.
      return;
    }
    KJ_CASE_ONEOF(r, Released) {
      // Do nothing in this case.
      return;
    }
  }
  KJ_UNREACHABLE;
}

jsg::MemoizedIdentity<jsg::Promise<void>>& WritableStreamDefaultWriter::getClosed() {
  return KJ_ASSERT_NONNULL(closedPromise, "the writer was never attached to a stream");
}

kj::Maybe<int> WritableStreamDefaultWriter::getDesiredSize(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this writer was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      return stream->getController().getDesiredSize();
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return 0;
    }
    KJ_CASE_ONEOF(r, Released) {
      JSG_FAIL_REQUIRE(TypeError, "This WritableStream writer has been released.");
    }
  }
  KJ_UNREACHABLE;
}

jsg::MemoizedIdentity<jsg::Promise<void>>& WritableStreamDefaultWriter::getReady() {
  return KJ_ASSERT_NONNULL(readyPromise, "the writer was never attached to a stream");
}

void WritableStreamDefaultWriter::lockToStream(jsg::Lock& js, WritableStream& stream) {
  KJ_ASSERT(!stream.isLocked());
  KJ_ASSERT(stream.getController().lockWriter(js, *this));
}

void WritableStreamDefaultWriter::releaseLock(jsg::Lock& js) {
  // TODO(soon): Releasing the lock should cancel any pending writes.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this writer was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      stream->getController().releaseWriter(*this, js);
      state.init<Released>();
      return;
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      // Do nothing in this case
      return;
    }
    KJ_CASE_ONEOF(r, Released) {
      // Do nothing in this case
      return;
    }
  }
  KJ_UNREACHABLE;
}

void WritableStreamDefaultWriter::replaceReadyPromise(jsg::Promise<void> readyPromise) {
  this->readyPromise = kj::mv(readyPromise);
}

jsg::Promise<void> WritableStreamDefaultWriter::write(jsg::Lock& js, v8::Local<v8::Value> chunk) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this writer was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      return stream->getController().write(js, chunk);
    }
    KJ_CASE_ONEOF(r, Released) {
      return js.rejectedPromise<void>(
          js.v8TypeError("This WritableStream writer has been released."_kj));
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return js.rejectedPromise<void>(
          js.v8TypeError("This WritableStream has been closed."_kj));
    }
  }
  KJ_UNREACHABLE;
}

void WritableStreamDefaultWriter::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(closedPromise, readyPromise);
}

// ======================================================================================

WritableStream::WritableStream(
    IoContext& ioContext,
    kj::Own<WritableStreamSink> sink,
    kj::Maybe<uint64_t> maybeHighWaterMark)
    : WritableStream(newWritableStreamInternalController(ioContext, kj::mv(sink),
                                                         maybeHighWaterMark)) {}

WritableStream::WritableStream(kj::Own<WritableStreamController> controller)
    : ioContext(tryGetIoContext()),
      controller(kj::mv(controller)) {
  getController().setOwnerRef(*this);
}

WritableStreamController& WritableStream::getController() { return *controller; }

kj::Own<WritableStreamSink> WritableStream::removeSink(jsg::Lock& js) {
  return JSG_REQUIRE_NONNULL(
      getController().removeSink(js),
      TypeError,
      "This WritableStream does not have a WritableStreamSink");
}

jsg::Promise<void> WritableStream::abort(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> reason) {
  if (isLocked()) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This WritableStream is currently locked to a writer."_kj));
  }
  return getController().abort(js, reason);
}

jsg::Promise<void> WritableStream::close(jsg::Lock& js) {
  if (isLocked()) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This WritableStream is currently locked to a writer."_kj));
  }
  return getController().close(js);
}

jsg::Promise<void> WritableStream::flush(jsg::Lock& js) {
  if (isLocked()) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This WritableStream is currently locked to a writer."_kj));
  }
  return getController().flush(js);
}

jsg::Ref<WritableStreamDefaultWriter> WritableStream::getWriter(jsg::Lock& js) {
  return WritableStreamDefaultWriter::constructor(js, JSG_THIS);
}

jsg::Ref<WritableStream> WritableStream::constructor(
    jsg::Lock& js,
    jsg::Optional<UnderlyingSink> underlyingSink,
    jsg::Optional<StreamQueuingStrategy> queuingStrategy,
    CompatibilityFlags::Reader flags) {
  JSG_REQUIRE(flags.getStreamsJavaScriptControllers(),
               Error,
               "To use the new WritableStream() constructor, enable the "
               "streams_enable_constructors feature flag.");
  auto stream = jsg::alloc<WritableStream>(newWritableStreamJsController());
  stream->getController().setup(js, kj::mv(underlyingSink), kj::mv(queuingStrategy));
  return kj::mv(stream);
}

}  // namespace workerd::api
