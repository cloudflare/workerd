// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/blob.h>
#include <workerd/api/js-readable-stream.h>
#include <workerd/api/streams/readable-source.h>
#include <workerd/api/url-standard.h>
#include <workerd/api/url.h>

#include <kj/debug.h>

namespace workerd::api {

namespace {
// The exact error message produced when attempting to consume an already-consumed body. This
// text is user-visible and load-bearing: it matches the message historically produced by the
// Body mixin (see http.c++) and must not change.
constexpr kj::StringPtr kBodyUsedError =
    "Body has already been used. It can only be used once. Use tee() first if you need to "
    "read it twice."_kj;

// Convert a kj::String to owned bytes, excluding the trailing NUL terminator. The returned array
// owns the original character buffer but views only the string's bytes.
kj::Array<const kj::byte> stringToBytes(kj::String data) {
  size_t len = data.size();
  auto chars = data.releaseArray();
  return chars.first(len).asBytes().asConst().attach(kj::mv(chars));
}

// Copy the bytes viewed by a JsBufferSource into an owned array. Copying (rather than retaining the
// JsBufferSource) severs the dependency on the V8 backing store, which could otherwise be freed if
// the source ArrayBuffer is detached/transferred and then garbage collected. This also satisfies
// the Fetch requirement to copy the input buffer.
kj::Array<const kj::byte> bufferSourceToBytes(
    jsg::Lock& js, jsg::JsRef<jsg::JsBufferSource>& view) {
  auto copy = kj::heapArray<kj::byte>(view.getHandle(js).asArrayPtr());
  return copy.asPtr().asConst().attach(kj::mv(copy));
}
}  // namespace

// The -Wdangling-field warnings below are false positives. view captures the heap buffer pointer
// managed by data, not the address of the data parameter itself. Moving data into owned transfers
// ownership of that heap buffer without changing its address, so view remains valid.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdangling-field"
JsReadableStream::Buffer::Buffer(kj::Array<const kj::byte> data): view(data), owned(kj::mv(data)) {}

JsReadableStream::Buffer::Buffer(jsg::Ref<Blob> data): view(data->getData()), owned(kj::mv(data)) {}
#pragma clang diagnostic pop

JsReadableStream::Impl JsReadableStream::bufferBackedImpl(jsg::Lock& js, kj::Rc<Buffer> buffer) {
  // Use streams::newMemorySource() rather than newSystemStream() wrapping a memory input stream:
  // buffer-backed bodies may have V8 heap provenance and therefore must NOT be deferred-proxied.
  // The data must be consumed and destroyed while under the isolate lock.
  //
  // TODO(streams-ts): Like create(), the stream construction here must dispatch on the
  // worker's configuration once the TypeScript implementation lands.
  auto view = buffer->view;
  auto source = streams::newMemorySource(view, buffer.addRef().toOwn());
  return Impl{
    .stream = StreamImpl(js.alloc<ReadableStream>(IoContext::current(), kj::mv(source))),
    .maybeOwnedBuffer = kj::mv(buffer),
  };
}

JsReadableStream::JsReadableStream(jsg::Ref<ReadableStream> stream)
    : impl(Impl{
        .stream = StreamImpl(kj::mv(stream)),
        .maybeOwnedBuffer = kj::none,
      }) {}

JsReadableStream::JsReadableStream(jsg::Lock&, jsg::JsRef<jsg::JsObject>) {
  KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
}

JsReadableStream::JsReadableStream(jsg::Lock& js, kj::Array<const kj::byte> data)
    : impl(bufferBackedImpl(js, kj::rc<Buffer>(kj::mv(data)))) {}

JsReadableStream::JsReadableStream(jsg::Lock& js, kj::String data)
    : JsReadableStream(js, stringToBytes(kj::mv(data))) {}

JsReadableStream::JsReadableStream(jsg::Lock& js, jsg::JsRef<jsg::JsBufferSource> view)
    : JsReadableStream(js, bufferSourceToBytes(js, view)) {}

JsReadableStream::JsReadableStream(jsg::Lock& js, jsg::Ref<Blob> blob)
    : impl(bufferBackedImpl(js, kj::rc<Buffer>(kj::mv(blob)))) {}

JsReadableStream::JsReadableStream(jsg::Lock& js, jsg::Ref<api::URLSearchParams> urlSearchParams)
    : JsReadableStream(js, urlSearchParams->toString()) {}

JsReadableStream::JsReadableStream(
    jsg::Lock& js, jsg::Ref<api::url::URLSearchParams> urlSearchParams)
    : JsReadableStream(js, urlSearchParams->toString()) {}

JsReadableStream JsReadableStream::create(
    jsg::Lock& js, IoContext& ioContext, kj::Own<ReadableStreamSource> source) {
  // TODO(streams-ts): Dispatch on the worker's configuration to construct either the legacy
  // C++ ReadableStream or a TypeScript-backed stream.
  return JsReadableStream(js.alloc<ReadableStream>(ioContext, kj::mv(source)));
}

JsReadableStream JsReadableStream::addRef(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        return JsReadableStream(Impl{
          .stream = StreamImpl(stream.addRef()),
          .maybeOwnedBuffer = i.maybeOwnedBuffer.map([](kj::Rc<Buffer>& b) { return b.addRef(); }),
        });
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }
  // addRef() of a null stream is a null stream.
  return JsReadableStream();
}

bool JsReadableStream::isNull() const {
  return impl == kj::none;
}

bool JsReadableStream::isBufferBacked() const {
  KJ_IF_SOME(i, impl) {
    return i.maybeOwnedBuffer != kj::none;
  }
  return false;
}

bool JsReadableStream::isDisturbed(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        return stream->isDisturbed();
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }
  return false;
}

bool JsReadableStream::isLocked(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        return stream->isLocked();
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }
  return false;
}

jsg::Promise<void> JsReadableStream::cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        return stream->cancel(js, kj::mv(reason));
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }
  // Canceling a null stream is a no-op.
  return js.resolvedPromise();
}

jsg::Promise<void> JsReadableStream::forceCancel(
    jsg::Lock& js, jsg::Optional<jsg::JsValue> reason) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        // Going through the controller (rather than ReadableStream::cancel()) deliberately
        // bypasses the "is locked" check: this cancels the stream out from under any reader.
        return stream->getController().cancel(js, kj::mv(reason));
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }
  // Canceling a null stream is a no-op.
  return js.resolvedPromise();
}

void JsReadableStream::setPendingClosure(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        stream->getController().setPendingClosure();
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
      }
    }
  }
}

jsg::Promise<void> JsReadableStream::onEof(jsg::Lock& js) {
  auto& i = KJ_ASSERT_NONNULL(impl, "onEof() called on a null JsReadableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
      return stream->onEof(js);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<uint64_t> JsReadableStream::tryGetLength(jsg::Lock& js, StreamEncoding encoding) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        return stream->tryGetLength(encoding);
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }
  return kj::none;
}

jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> JsReadableStream::arrayBuffer(
    jsg::Lock& js, uint64_t limit) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        if (stream->isDisturbed()) {
          return js.rejectedPromise<jsg::JsRef<jsg::JsArrayBuffer>>(js.typeError(kBodyUsedError));
        }
        return stream->getController().readAllBytes(js, limit);
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }

  // A null stream yields an empty result.
  return js.resolvedPromise(jsg::JsArrayBuffer::create(js, 0).addRef(js));
}

jsg::Promise<kj::String> JsReadableStream::text(jsg::Lock& js, uint64_t limit) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        if (stream->isDisturbed()) {
          return js.rejectedPromise<kj::String>(js.typeError(kBodyUsedError));
        }
        return stream->getController().readAllText(js, limit);
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }

  // A null stream yields an empty result.
  return js.resolvedPromise(kj::String());
}

jsg::Promise<jsg::JsRef<jsg::JsUint8Array>> JsReadableStream::bytes(jsg::Lock& js, uint64_t limit) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        if (stream->isDisturbed()) {
          return js.rejectedPromise<jsg::JsRef<jsg::JsUint8Array>>(js.typeError(kBodyUsedError));
        }
        return stream->getController().readAllBytes(js, limit).then(
            js, [](jsg::Lock& js, jsg::JsRef<jsg::JsArrayBuffer> data) {
          auto handle = data.getHandle(js);
          return jsg::JsUint8Array::create(js, handle).addRef(js);
        });
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }

  // A null stream yields an empty result.
  return js.resolvedPromise(jsg::JsUint8Array::create(js, static_cast<size_t>(0)).addRef(js));
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> JsReadableStream::json(jsg::Lock& js, uint64_t limit) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        if (stream->isDisturbed()) {
          return js.rejectedPromise<jsg::JsRef<jsg::JsValue>>(js.typeError(kBodyUsedError));
        }
        return stream->getController().readAllText(js, limit).then(
            js, [](jsg::Lock& js, kj::String text) {
          auto parsed = js.parseJson(text);
          return jsg::JsValue(parsed.getHandle(js)).addRef(js);
        });
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }

  // A null stream is an empty body. Match Body::json() semantics exactly: resolve the empty
  // text first, then parse it in a continuation. Parsing "" as JSON throws a SyntaxError, so
  // the returned promise rejects.
  return js.resolvedPromise(kj::String()).then(js, [](jsg::Lock& js, kj::String text) {
    auto parsed = js.parseJson(text);
    return jsg::JsValue(parsed.getHandle(js)).addRef(js);
  });
}

jsg::Promise<jsg::Ref<Blob>> JsReadableStream::blob(
    jsg::Lock& js, uint64_t limit, kj::String contentType) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        if (stream->isDisturbed()) {
          return js.rejectedPromise<jsg::Ref<Blob>>(js.typeError(kBodyUsedError));
        }
        return stream->getController().readAllBytes(js, limit).then(js,
            [contentType = kj::mv(contentType)](
                jsg::Lock& js, jsg::JsRef<jsg::JsArrayBuffer> buffer) mutable {
          return js.alloc<Blob>(js, jsg::JsBufferSource(buffer.getHandle(js)), kj::mv(contentType));
        });
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }

  // A null stream yields an empty Blob with the given Content-Type.
  return js.resolvedPromise(js.alloc<Blob>(kj::mv(contentType)));
}

kj::Promise<DeferredProxy<void>> JsReadableStream::pumpTo(
    jsg::Lock& js, kj::Own<WritableStreamSink> sink, EndStream end) {
  auto& i = KJ_ASSERT_NONNULL(impl, "pumpTo() called on a null JsReadableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
      return stream->pumpTo(js, kj::mv(sink), end == EndStream::YES);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
    }
  }
  KJ_UNREACHABLE;
}

JsReadableStream::Tee JsReadableStream::tee(jsg::Lock& js) {
  auto& i = KJ_ASSERT_NONNULL(impl, "tee() called on a null JsReadableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
      auto branches = stream->tee(js);
      // Both branches share the retransmit buffer (if any) so they remain independently rewindable.
      auto buffer1 = i.maybeOwnedBuffer.map([](kj::Rc<Buffer>& b) { return b.addRef(); });
      auto buffer2 = i.maybeOwnedBuffer.map([](kj::Rc<Buffer>& b) { return b.addRef(); });

      // tee() consumes `this`: the original stream is now locked/disturbed and unusable, so
      // represent that by nullifying. Everything needed for the result has already been extracted
      // into locals above.
      impl = kj::none;

      return Tee{
        .branch1 = JsReadableStream(Impl{
          .stream = StreamImpl(kj::mv(branches[0])),
          .maybeOwnedBuffer = kj::mv(buffer1),
        }),
        .branch2 = JsReadableStream(Impl{
          .stream = StreamImpl(kj::mv(branches[1])),
          .maybeOwnedBuffer = kj::mv(buffer2),
        }),
      };
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<JsReadableStream> JsReadableStream::tryClone(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_IF_SOME(buffer, i.maybeOwnedBuffer) {
      // Non-mutating: build a fresh stream over the same buffer bytes, carrying the buffer forward
      // so the clone is itself rewindable. `this` is left untouched.
      return JsReadableStream(bufferBackedImpl(js, buffer.addRef()));
    }
  }
  // Null or not buffer-backed: nothing to rewind.
  return kj::none;
}

JsReadableStream JsReadableStream::detach(jsg::Lock& js, IgnoreDisturbed ignoreDisturbed) {
  auto& i = KJ_ASSERT_NONNULL(impl, "detach() called on a null JsReadableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
      // ReadableStream::detach() takes over the internal state, leaving the original stream (i.e.
      // `this`) locked and disturbed. `this` retains a reference to the retransmit buffer.
      auto detached = stream->detach(js, ignoreDisturbed == IgnoreDisturbed::YES);
      return JsReadableStream(Impl{
        .stream = StreamImpl(kj::mv(detached)),
        .maybeOwnedBuffer = i.maybeOwnedBuffer.map([](kj::Rc<Buffer>& b) { return b.addRef(); }),
      });
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
    }
  }
  KJ_UNREACHABLE;
}

void JsReadableStream::nullify() {
  impl = kj::none;
}

void JsReadableStream::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        visitor.visit(stream);
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        visitor.visit(obj);
      }
    }
    // Note: the retransmit Buffer's owned Blob reference is intentionally NOT traced here, matching
    // the pre-refactor Body behavior (it is kept alive as a strong reference, not via GC tracing).
  }
}

void JsReadableStream::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        tracker.trackField("stream", stream);
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        // TODO(streams-ts): track the JS object's memory once TS-backed streams are supported.
        // (jsg::JsRef does not satisfy the MemoryRetainer concept, so it can't be passed to
        // trackField() directly.)
      }
    }
    KJ_IF_SOME(buffer, i.maybeOwnedBuffer) {
      tracker.trackFieldWithSize("buffer", buffer->view.size());
    }
  }
}

}  // namespace workerd::api
