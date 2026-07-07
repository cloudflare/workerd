// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/streams/writable.h>

#include <kj/common.h>
#include <kj/one-of.h>

namespace workerd::api {

// An abstraction of a WritableStream, backed by either a C++ implemented WritableStream
// (defined in src/workerd/api/streams/*) or a TypeScript implemented WritableStream (defined
// in src/per_isolate/webstreams). The API is limited strictly to the methods that are needed
// by the C++ side of workerd. It is not intended to be a complete implementation of the
// WritableStream API. This is the writable-side counterpart of JsReadableStream (see
// js-readable-stream.h); the two follow the same conventions.
//
// A JsWritableStream is one of:
//   * null / empty (isNull()) -- the default-constructed or moved-from state; or
//   * stream-backed -- wraps a WritableStream.
//
// Unlike JsReadableStream there is no buffer-backed (rewindable) state: writables have no
// retransmission concept. There is also no "disturbed" concept for writables.
//
// Backend branching: the underlying stream is stored as a kj::OneOf so that a TypeScript
// implemented WritableStream (represented as a JS object) can eventually be supported alongside
// the legacy C++ WritableStream. Every method that touches the underlying stream switches on the
// backend. Today only the C++ WritableStream backend is implemented; the TypeScript backend
// branches are KJ_UNIMPLEMENTED and the corresponding constructor cannot yet be used. When the
// TypeScript implementation lands, fill in those branches -- the stored type (and therefore the
// consumers) will not need to change.
class JsWritableStream final {
 public:
  // The underlying stream. Today only the legacy C++ WritableStream alternative is populated; the
  // jsg::JsRef<jsg::JsObject> alternative is reserved for the future TypeScript implementation.
  using StreamImpl = kj::OneOf<jsg::Ref<WritableStream>, jsg::JsRef<jsg::JsObject>>;

  struct Impl {
    StreamImpl stream;
  };

  // Create a null / empty JsWritableStream. Unlike JsReadableStream's null state (which
  // represents "no body" and is a real consumer-visible state), a null JsWritableStream is only
  // ever a default-constructed or moved-from artifact. State queries are null-safe; most
  // operations assert non-null (see the individual method comments).
  JsWritableStream() = default;

  // Adopt an existing legacy C++ WritableStream.
  JsWritableStream(jsg::Ref<WritableStream> stream);

  // Adopt a TypeScript-implemented WritableStream (a JS object). Not yet supported.
  JsWritableStream(jsg::Lock& js, jsg::JsRef<jsg::JsObject> obj);

  JsWritableStream(JsWritableStream&&) = default;
  JsWritableStream& operator=(JsWritableStream&&) = default;
  JsWritableStream(const JsWritableStream&) = delete;
  JsWritableStream& operator=(const JsWritableStream&) = delete;

  // Create a JsWritableStream wrapping the given native data sink. This is the canonical way for
  // C++ code to mint a new WritableStream to hand to JavaScript.
  //
  // The observer (used for byte stream metrics) is provided by the caller (typically
  // ioContext.getMetrics().tryCreateWritableByteStreamObserver()). maybeHighWaterMark configures
  // the internal write buffer's backpressure threshold. If maybeClosureWaitable is provided,
  // closing the stream will not complete until the given promise resolves (used by sockets to
  // gate closure on connection establishment).
  //
  // TODO(streams-ts): This is the future compatibility-flag dispatch point. Once the
  // TypeScript implementation lands, this will construct either the legacy C++ WritableStream
  // or a TypeScript-backed stream depending on the worker's configuration.
  static JsWritableStream create(jsg::Lock& js,
      IoContext& ioContext,
      kj::Own<WritableStreamSink> sink,
      kj::Maybe<kj::Own<ByteStreamObserver>> observer,
      kj::Maybe<uint64_t> maybeHighWaterMark = kj::none,
      kj::Maybe<jsg::Promise<void>> maybeClosureWaitable = kj::none);

  // Returns a new JsWritableStream sharing this one's underlying stream. Both instances observe
  // the same underlying stream state (e.g. the stream closing through one is visible through the
  // other), and passing either through the type wrapper yields the same JavaScript object. This
  // is what identity-preserving accessors (e.g. socket.writable === socket.writable) are built
  // from. addRef() of a null stream is a null stream.
  JsWritableStream addRef(jsg::Lock& js);

  // True if this is a null / empty stream. Inspects only C++-side state; a jsg::Lock& is not
  // required because it never dispatches to the JS backend.
  bool isNull() const;

  // True if the underlying stream is currently locked to a writer. A null stream is never
  // locked. Not const: for the C++ backend this dispatches to WritableStream::isLocked(), which
  // is non-const.
  bool isLocked(jsg::Lock& js);

  // True if the underlying stream is closed or in the process of closing. A null stream answers
  // false. Not const for the same reason as isLocked().
  bool isClosedOrClosing(jsg::Lock& js);

  // Waits for all pending writes to complete. Rejects if the stream is currently locked to a
  // writer, matching the (workerd-internal, non-standard) WritableStream::flush() extension.
  // Precondition: !isNull().
  jsg::Promise<void> flush(jsg::Lock& js);

  // Like flush(), but bypasses the writer-lock check by going through the controller. Used when
  // pending data must be flushed regardless of what JavaScript is doing with the stream (e.g.
  // when a Socket is being closed). Precondition: !isNull().
  jsg::Promise<void> forceFlush(jsg::Lock& js);

  // Immediately interrupts pending writes and errors the stream, bypassing the writer-lock check
  // by going through the controller. This is a forcible teardown used when the stream's
  // underlying connection is going away regardless of what JavaScript is doing with the stream
  // (e.g. a Socket's writable side when the socket is closed or errors). Force-aborting a null
  // stream is a no-op (resolved promise).
  jsg::Promise<void> forceAbort(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason);

  // Closes the stream once all pending writes complete, bypassing the writer-lock check by going
  // through the controller. Used when the stream must be closed regardless of what JavaScript is
  // doing with it (e.g. closing a Socket's write side when its readable side reaches EOF).
  // Precondition: !isNull().
  jsg::Promise<void> forceClose(jsg::Lock& js);

  // Mark the stream as being in the process of shutting down (e.g. the Socket it belongs to is
  // closing), before the closure has actually completed. A no-op on a null stream.
  void setPendingClosure(jsg::Lock& js);

  // Detach the underlying stream from its implementation, leaving it permanently locked and
  // unusable for further writes. Unlike JsReadableStream::detach(), nothing is returned: the
  // stream is simply neutralized in place (used when the underlying connection is taken over by
  // another consumer, e.g. startTls or Socket::takeConnectionStream). Throws if the stream is
  // locked to a writer or is closed/closing; if the stream is errored, throws the stored error.
  // Precondition: !isNull().
  void detach(jsg::Lock& js);

  void visitForGc(jsg::GcVisitor& visitor);
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

  // Describe this type to RTTI (and therefore to generated TypeScript) exactly as a
  // WritableStream. See the delegated-RTTI support in jsg/rtti.h.
  using JsgRttiDelegate = jsg::Ref<WritableStream>;

  static v8::Local<v8::Value> jsgWrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      JsWritableStream stream) {
    // Wrapping a null JsWritableStream indicates a bug: APIs that can produce "no stream"
    // express that as kj::Maybe<JsWritableStream> / jsg::Optional<JsWritableStream> so that
    // absence maps to JS null/undefined rather than to a fabricated stream.
    auto& impl = KJ_ASSERT_NONNULL(stream.impl, "cannot wrap a null JsWritableStream");
    KJ_SWITCH_ONEOF(impl.stream) {
      KJ_CASE_ONEOF(legacy, jsg::Ref<WritableStream>) {
        return typeWrapper.wrap(js, context, creator, kj::mv(legacy));
      }
      KJ_CASE_ONEOF(ts, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript WritableStream not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }

  static kj::Maybe<JsWritableStream> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // For now, we only support unwrapping the legacy C++ WritableStream.
    // Later we will also support the TypeScript implementation.
    return typeWrapper.tryUnwrap(
        js, context, handle, static_cast<jsg::Ref<WritableStream>*>(nullptr), parentObject);
  }

 private:
  explicit JsWritableStream(Impl impl): impl(kj::mv(impl)) {}

  kj::Maybe<Impl> impl;
};

}  // namespace workerd::api
