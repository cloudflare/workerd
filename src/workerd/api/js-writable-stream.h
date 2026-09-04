// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/js-readable-stream.h>
#include <workerd/api/streams/transform.h>
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
// Backend branching: the underlying stream is stored as a kj::OneOf -- either the legacy C++
// WritableStream or a TypeScript implemented WritableStream (represented as a JS object). Every
// method that touches the underlying stream switches on the backend. The TypeScript arms
// dispatch through the webstreams/cpp_exports bootstrap module (see js-writable-stream.c++),
// which exists only when the typescript_implemented_streams compat flag (plus the per-isolate
// bootstrap autogate) is enabled.
class JsWritableStream final {
 public:
  // The underlying stream: either the legacy C++ WritableStream or the TypeScript
  // implementation's stream object.
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

  // Adopt a TypeScript-implemented WritableStream (a JS object). No brand validation happens
  // here -- the caller asserts the object really is one. Untrusted values arrive through
  // jsgTryUnwrap/tryUnwrapTs, which brand-check before adopting; internal callers (create())
  // construct from a known-good conduit instance.
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
  // This is the compatibility-flag dispatch point: when the typescript_implemented_streams
  // compat flag is enabled, the sink is wrapped in a WritableStreamNativeSink and the stream is
  // constructed by the TypeScript implementation; otherwise the legacy C++ WritableStream is
  // used.
  static JsWritableStream create(jsg::Lock& js,
      IoContext& ioContext,
      kj::Own<WritableStreamSink> sink,
      kj::Maybe<kj::Own<ByteStreamObserver>> observer,
      kj::Maybe<uint64_t> maybeHighWaterMark = kj::none,
      kj::Maybe<jsg::Promise<void>> maybeClosureWaitable = kj::none);

  // Create a JsWritableStream whose write() calls are driven directly by the given C++
  // `write` function: each chunk passed to writer.write() is handed to `write` as-is, with
  // no byte-level coalescing or splitting. This is for sinks that consume whole JS values
  // per write() call rather than bytes (e.g. UDP's Datagram objects, where each write() call
  // must map to exactly one outbound packet).
  //
  // This is a compatibility-flag dispatch point: under
  // typescript_implemented_streams, `write` is wrapped as a real JS function and the
  // TypeScript stream is constructed over a plain (non-native-marked) underlying sink;
  // otherwise this builds the legacy C++ WritableStream directly.
  static JsWritableStream fromWrite(
      jsg::Lock& js, kj::Function<jsg::Promise<void>(jsg::Lock&, jsg::JsValue)> write);

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

  // Enqueue a write through the stream's standard write machinery, returning a promise that
  // settles when the write's I/O completes. FOR TESTS ONLY: this exists so that tests of
  // consumers (e.g. sockets-test.c++'s output-gate tests) can drive writes without reaching
  // into a backend-specific controller. The legacy arm writes through the controller
  // directly; the TypeScript arm acquires the writer, writes, and releases it, so the
  // stream must not be locked. Production code must never call this. Precondition:
  // !isNull().
  jsg::Promise<void> writeForTest(jsg::Lock& js, jsg::JsValue chunk);

  // Serialize the stream for RPC transfer, exactly like WritableStream::serialize(): the peer's
  // ByteStream is adopted as the stream's sink and an external table entry describing it is written
  // to the serializer. Used by consumers that transfer a stream over RPC (e.g. Socket).
  // Precondition: !isNull().
  void serialize(jsg::Lock& js, jsg::Serializer& serializer);

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
        // The TypeScript-implemented stream IS a JS object; wrapping just hands the same
        // handle back, which is what preserves identity (socket.writable === socket.writable).
        return ts.getHandle(js);
      }
    }
    KJ_UNREACHABLE;
  }

  static kj::Maybe<JsWritableStream> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    KJ_IF_SOME(legacy,
        typeWrapper.tryUnwrap(
            js, context, handle, static_cast<jsg::Ref<WritableStream>*>(nullptr), parentObject)) {
      return JsWritableStream(kj::mv(legacy));
    }
    // TypeScript-implemented streams are plain JS objects with no JSG wrapper, so the
    // typeWrapper cannot recognize them; ask the TS implementation's own brand check.
    return tryUnwrapTs(js, handle);
  }

  // The TypeScript arm of jsgTryUnwrap: recognizes a TypeScript-implemented WritableStream
  // by the implementation's private brand (via the bootstrap bridge's isWritableStream) and
  // adopts it. Returns kj::none if the typescript_implemented_streams compat flag is off
  // (the bootstrap export does not exist then) or if the value is not a TS stream.
  //
  // Deliberately performs no locked/closing checks, matching the legacy arm: unwrap adopts
  // the handle as-is and consumers enforce their own preconditions. Public so tests can
  // drive it directly; production code goes through jsgTryUnwrap.
  static kj::Maybe<JsWritableStream> tryUnwrapTs(jsg::Lock& js, v8::Local<v8::Value> handle);

  // Return the underlying stream if it is backed by the given arm, kj::none otherwise
  // (including when this is a null stream). Used by JsReadableStream's pipe dispatch cells;
  // production code outside the pipe dispatch should not inspect arms (see the dispatch
  // guardrails in the design doc).
  kj::Maybe<jsg::Ref<WritableStream>> tryGetLegacy(jsg::Lock& js);
  kj::Maybe<jsg::JsObject> tryGetTs(jsg::Lock& js);

 private:
  explicit JsWritableStream(Impl impl): impl(kj::mv(impl)) {}

  kj::Maybe<Impl> impl;

  // JsReadableStream::pipeTo()/pipeThrough() dispatch on the backend of both pipe ends, which
  // requires access to the destination's internal arm.
  friend class JsReadableStream;
};

// A transform endpoint pair: a readable side and a writable side, typically (but not
// necessarily) the two ends of a TransformStream, without prescribing which backend implements
// them. This is the abstraction-level equivalent of the spec's ReadableWritablePair dictionary
// (and of the ReadableStream::Transform JSG_STRUCT), and is the argument type of
// JsReadableStream::pipeThrough().
struct JsReadableWritablePair {
  JsReadableStream readable;
  JsWritableStream writable;

  // Describe this type to RTTI (and therefore to generated TypeScript) exactly as
  // ReadableStream::Transform (whose TS override is ReadableWritablePair). See the
  // delegated-RTTI support in jsg/rtti.h.
  using JsgRttiDelegate = ReadableStream::Transform;

  static v8::Local<v8::Value> jsgWrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      JsReadableWritablePair pair) {
    // Dictionary semantics: produce a plain { readable, writable } object, wrapping each member
    // through its own conversion. Null members trip the members' own null-wrap asserts.
    auto obj = js.obj();
    obj.set(js, "readable"_kj,
        jsg::JsValue(typeWrapper.wrap(js, context, creator, kj::mv(pair.readable))));
    obj.set(js, "writable"_kj,
        jsg::JsValue(typeWrapper.wrap(js, context, creator, kj::mv(pair.writable))));
    return obj;
  }

  static kj::Maybe<JsReadableWritablePair> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // Tier 1 (brand-first): a genuine C++ TransformStream (or subclass, e.g.
    // IdentityTransformStream) is used directly via its C++ accessors. No JS property reads
    // occur, so instance-shadowed readable/writable getters are ignored -- exactly the semantics
    // a jsg::Ref<TransformStream> parameter has today.
    KJ_IF_SOME(transform,
        typeWrapper.tryUnwrap(
            js, context, handle, static_cast<jsg::Ref<TransformStream>*>(nullptr), parentObject)) {
      return JsReadableWritablePair{
        .readable = JsReadableStream(transform->getReadable()),
        .writable = JsWritableStream(transform->getWritable()),
      };
    }

    // Tier 2 (dictionary-shaped fallback): read the readable/writable properties (in
    // ReadableStream::Transform field order) and unwrap each through the member abstractions'
    // own conversions. This is what keeps the pair backend-agnostic: brand checks live in the
    // members (including, later, the TypeScript implementation's). Either member failing fails
    // the whole unwrap.
    if (!handle->IsObject()) {
      return kj::none;
    }
    auto obj = jsg::JsObject(handle.As<v8::Object>());
    auto readable = obj.get(js, "readable"_kj);
    auto writable = obj.get(js, "writable"_kj);
    KJ_IF_SOME(r,
        typeWrapper.tryUnwrap(
            js, context, readable, static_cast<JsReadableStream*>(nullptr), parentObject)) {
      KJ_IF_SOME(w,
          typeWrapper.tryUnwrap(
              js, context, writable, static_cast<JsWritableStream*>(nullptr), parentObject)) {
        return JsReadableWritablePair{
          .readable = kj::mv(r),
          .writable = kj::mv(w),
        };
      }
    }
    return kj::none;
  }

  void visitForGc(jsg::GcVisitor& visitor);
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;
};

// The C++ implementation of the "native underlying sink" contract defined by the
// TypeScript streams implementation (see the writable-side markers in
// src/per_isolate/webstreams/native.ts).
//
// A WritableStreamNativeSink wraps a kj-native WritableStreamSink so that a
// TypeScript-implemented WritableStream can be backed directly by a C++ byte sink. Unlike
// the readable side's pull conduit, no new backend machinery exists on the TS side: the
// standard WritableStream machinery drives this object through the ordinary UnderlyingSink
// hooks (write/close/abort), one operation at a time. The marker symbol exists for pipe
// dispatch and extraction; the one extension hook is pipeFrom(source, options), which the
// TS pipeTo calls when BOTH pipe ends are native-backed so the pipe runs entirely at the
// C++ layer.
//
// Instances are created only from C++ (there is no JavaScript constructor) and are handed
// to the TypeScript WritableStream constructor carrying the native-sink marker symbol.
// The object is never exposed to user code, and it is suppressed from the generated
// TypeScript types.
class WritableStreamNativeSink final: public jsg::Object {
 public:
  WritableStreamNativeSink(IoContext& ioContext,
      kj::Own<WritableStreamSink> sink,
      kj::Maybe<kj::Own<ByteStreamObserver>> observer,
      kj::Maybe<jsg::Promise<void>> maybeClosureWaitable);

  // The standard UnderlyingSink write hook. Accepts the same chunk types as the legacy
  // internal controller (ArrayBuffer, SharedArrayBuffer, ArrayBufferView, and -- as the
  // same ergonomic extension -- strings, encoded as UTF-8); anything else rejects with the
  // legacy "only supports writing byte types" TypeError, which errors the stream. The
  // bytes are copied to KJ-owned memory under the isolate lock before the I/O write (the
  // source buffer may be detached/resized while the write is in flight). Undefined and
  // empty chunks resolve without touching the sink.
  jsg::Promise<void> write(jsg::Lock& js, jsg::Optional<jsg::JsValue> chunk);

  // The standard UnderlyingSink close hook: ends the sink. If a closure waitable was
  // provided at construction, closing waits for it first; if the waitable REJECTS, the
  // close resolves WITHOUT ending the sink (the teardown is reported through the owning
  // object's own promises instead -- e.g. a Socket's closed/opened), mirroring the legacy
  // controller's closure-waitable quirk. The sink is released either way.
  jsg::Promise<void> close(jsg::Lock& js);

  // The standard UnderlyingSink abort hook: aborts the sink and releases it. If a write's
  // I/O is somehow still in flight (the TS machinery serializes sink operations, so this
  // is defensive), the release is deferred to the write's settlement.
  jsg::Promise<void> abort(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason);

  // The native+native pipe fast path, called by the TS pipeTo dispatch when both ends
  // carry extraction markers, no prevent* option is set, and both endpoints are in their
  // normal flowing states (the dispatch routes everything else to the JS pump, which can
  // honor post-pipe endpoint usability and stored-error rejections; `this` is the
  // extracted sink and `source` the extracted ReadableStreamNativeSource). Consumes both
  // endpoints and runs the pump entirely at the C++ layer. The options arrive
  // pre-converted and pre-validated by the dispatch as plain data properties; the
  // prevent* handling here implements the intended fast-path semantics but only sees
  // false values under the current dispatch gate. The returned promise is what
  // ReadableStream.prototype.pipeTo resolves.
  jsg::Promise<void> pipeFrom(jsg::Lock& js, jsg::JsObject sourceObj, jsg::JsObject optionsObj);

  // The C++ half of JsWritableStream::detach()'s TS arm, called by the TypeScript
  // detachWritableStream just before it drops its reference to this object: releases the
  // owned sink WITHOUT ending or aborting it -- the underlying connection is being taken
  // over by other code (e.g. Socket startTls / takeConnectionStream) -- matching the
  // legacy controller's detach, which drops its sink outright. Without this, the
  // controller's stored hook algorithms (which close over this object) would keep the
  // taken-over connection's sink alive until the stream is GC'd.
  void detach(jsg::Lock& js);

  JSG_RESOURCE_TYPE(WritableStreamNativeSink) {
    JSG_PRIVATE_SYMBOL(kNativeSink);
    JSG_METHOD(write);
    JSG_METHOD(close);
    JSG_METHOD(abort);
    JSG_METHOD(pipeFrom);
    JSG_METHOD(detach);

    // Internal plumbing type: keep it out of the generated TypeScript types.
    JSG_TS_OVERRIDE(type WritableStreamNativeSink = never);
  }

  void visitForGc(jsg::GcVisitor& visitor);
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  struct Active {
    IoOwn<WritableStreamSink> sink;
  };

  // Ends the sink and releases it (the post-waitable phase of close()).
  jsg::Promise<void> closeImpl(jsg::Lock& js);

  // kj::none once the sink has been ended, aborted, or consumed by pipeFrom().
  kj::Maybe<Active> state;

  // Byte stream metrics. Reported per chunk: onChunkEnqueued when the write hook accepts
  // the chunk, onChunkDequeued when its I/O settles. (The queue itself lives on the TS
  // side, so unlike the legacy controller at most one chunk is "enqueued" at a time.)
  kj::Maybe<kj::Own<ByteStreamObserver>> observer;

  // If present, close() must not complete before this resolves (used by sockets to gate
  // closure on connection establishment). Consumed by the first close().
  kj::Maybe<jsg::Promise<void>> maybeClosureWaitable;

  // Defensive only: the TS machinery serializes sink operations (at most one write or
  // close in flight).
  bool writeInFlight = false;

  // True while closeImpl()'s end() is outstanding (including while parked on the actor
  // output gate). Unlike writeInFlight this is not merely defensive: pipeFrom() extraction
  // bypasses the sink-hook serialization (the TS pipe dispatch rejects close-queued
  // destinations, but the sink's preconditions must not depend on that gate), and the
  // in-flight end() references the sink, so moving it into a pump would be a
  // use-after-free.
  bool closeInFlight = false;

  // Set when abort() arrives while a write's I/O is in flight: the sink's release is
  // deferred to the write's settlement.
  bool pendingAbort = false;
};

}  // namespace workerd::api
