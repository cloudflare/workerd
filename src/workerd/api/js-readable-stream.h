// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/deferred-proxy.h>
#include <workerd/api/streams/readable.h>
#include <workerd/util/strong-bool.h>

#include <kj/common.h>
#include <kj/one-of.h>
#include <kj/refcount.h>

namespace workerd::api {

class Blob;
class FormData;
class JsWritableStream;
struct JsReadableWritablePair;
class URLSearchParams;
namespace url {
class URLSearchParams;
}

WD_STRONG_BOOL(EndStream);
WD_STRONG_BOOL(IgnoreDisturbed);

// An abstraction of a ReadableStream, backed by either a C++ implemented ReadableStream
// (defined in src/workerd/api/streams/*) or a TypeScript implemented ReadableStream (defined
// in src/per_isolate/webstreams). The API is limited strictly to the methods that are needed
// by the C++ side of workerd. It is not intended to be a complete implementation of the
// ReadableStream API.
//
// A JsReadableStream is one of:
//   * null / empty (isNull()) -- represents "no body";
//   * buffer-backed -- wraps an in-memory data source that can be rewound and retransmitted
//     (isBufferBacked()); or
//   * stream-backed -- wraps an opaque, one-shot ReadableStream.
//
// Backend branching: the underlying stream is stored as a kj::OneOf so that a TypeScript
// implemented ReadableStream (represented as a JS object) can eventually be supported alongside
// the legacy C++ ReadableStream. Every method that touches the underlying stream switches on the
// backend. Today only the C++ ReadableStream backend is implemented; the TypeScript backend
// branches are KJ_UNIMPLEMENTED and the corresponding constructor cannot yet be used. When the
// TypeScript implementation lands, fill in those branches -- the stored type (and therefore
// Body / ExtractedBody) will not need to change.
class JsReadableStream final {
 public:
  // The underlying stream. Today only the legacy C++ ReadableStream alternative is populated; the
  // jsg::JsRef<jsg::JsObject> alternative is reserved for the future TypeScript implementation.
  using StreamImpl = kj::OneOf<jsg::Ref<ReadableStream>, jsg::JsRef<jsg::JsObject>>;

  // Holds the in-memory data backing a buffer-backed (rewindable) JsReadableStream. Refcounted so
  // its lifetime can be tied both to the native stream (as the newMemorySource backing) and to the
  // JsReadableStream itself (so the stream can be rebuilt for retransmission).
  struct Buffer {
    kj::ArrayPtr<const kj::byte> view;
    kj::OneOf<kj::Array<const kj::byte>, jsg::Ref<Blob>> owned;

    explicit Buffer(kj::Array<const kj::byte> data);
    explicit Buffer(jsg::Ref<Blob> data);
  };

  struct Impl {
    StreamImpl stream;
    kj::Maybe<kj::Rc<Buffer>> maybeOwnedBuffer;
  };

  // The result of tee(): two independent branches that each read the same data.
  struct Tee;

  // Create a null / empty JsReadableStream.
  JsReadableStream() = default;

  // Adopt an existing legacy C++ ReadableStream. The result is not rewindable.
  JsReadableStream(jsg::Ref<ReadableStream> stream);

  // Adopt a TypeScript-implemented ReadableStream (a JS object). Not yet supported.
  JsReadableStream(jsg::Lock& js, jsg::JsRef<jsg::JsObject> obj);

  // Create a buffer-backed (rewindable) JsReadableStream from various in-memory data sources. The
  // kj::String, jsg::JsRef<jsg::JsBufferSource>, and URLSearchParams overloads are conveniences
  // that copy / serialize the source into owned bytes and delegate to the kj::Array overload. The
  // JsBufferSource overload copies (rather than retaining a reference to the view) so the resulting
  // stream is unaffected if the source ArrayBuffer is later detached or transferred.
  JsReadableStream(jsg::Lock& js, kj::Array<const kj::byte> data);
  JsReadableStream(jsg::Lock& js, kj::String data);
  JsReadableStream(jsg::Lock& js, jsg::JsRef<jsg::JsBufferSource> view);
  JsReadableStream(jsg::Lock& js, jsg::Ref<Blob> blob);
  JsReadableStream(jsg::Lock& js, jsg::Ref<api::URLSearchParams> urlSearchParams);
  JsReadableStream(jsg::Lock& js, jsg::Ref<api::url::URLSearchParams> urlSearchParams);

  JsReadableStream(JsReadableStream&&) = default;
  JsReadableStream& operator=(JsReadableStream&&) = default;
  JsReadableStream(const JsReadableStream&) = delete;
  JsReadableStream& operator=(const JsReadableStream&) = delete;

  // Create a stream-backed (non-rewindable) JsReadableStream wrapping the given native data
  // source. This is the canonical way for C++ code to mint a new ReadableStream to hand to
  // JavaScript.
  //
  // TODO(streams-ts): This is the future compatibility-flag dispatch point. Once the
  // TypeScript implementation lands, this will construct either the legacy C++ ReadableStream
  // or a TypeScript-backed stream depending on the worker's configuration.
  static JsReadableStream create(
      jsg::Lock& js, IoContext& ioContext, kj::Own<ReadableStreamSource> source);

  // Returns a new JsReadableStream sharing this one's underlying stream (and retransmit
  // buffer, if any). Both instances observe the same underlying stream state (e.g. the stream
  // becoming disturbed through one is visible through the other), and passing either through
  // the type wrapper yields the same JavaScript object. This is what identity-preserving
  // accessors (e.g. request.body === request.body) are built from. addRef() of a null stream
  // is a null stream.
  JsReadableStream addRef(jsg::Lock& js);

  // True if this is a null / empty stream ("no body"). Inspects only C++-side state; a
  // jsg::Lock& is not required because it never dispatches to the JS backend.
  bool isNull() const;

  // True if this is backed by an in-memory buffer, and therefore rewindable via tryClone().
  // Inspects only C++-side state; a jsg::Lock& is not required.
  bool isBufferBacked() const;

  // True if the stream has been read from / consumed at all. Not const: for the C++ backend this
  // dispatches to ReadableStream::isDisturbed(), which is non-const.
  bool isDisturbed(jsg::Lock& js);

  // True if the underlying stream is currently locked (to a reader, a pending pump, a tee,
  // etc.). A null stream is never locked. Not const for the same reason as isDisturbed().
  bool isLocked(jsg::Lock& js);

  // The known length of the stream for the given encoding, if known; kj::none otherwise. Not const
  // for the same reason as isDisturbed().
  kj::Maybe<uint64_t> tryGetLength(
      jsg::Lock& js, StreamEncoding encoding = StreamEncoding::IDENTITY);

  // Cancel the stream with the given reason, indicating a loss of interest in the data. The
  // stream is left disturbed and closed. Rejects if the stream is currently locked, matching
  // ReadableStream.prototype.cancel(). Canceling a null stream is a no-op (resolved promise).
  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason);

  // Cancel the stream even if it is currently locked. This is a forcible teardown used when the
  // stream's underlying connection is going away regardless of what JavaScript is doing with the
  // stream (e.g. a Socket's readable side when the socket is closed or errors). Force-canceling
  // a null stream is a no-op (resolved promise).
  jsg::Promise<void> forceCancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason);

  // Mark the stream as being in the process of shutting down (e.g. the Socket it belongs to is
  // closing), before the closure has actually completed. A no-op on a null stream.
  void setPendingClosure(jsg::Lock& js);

  // Returns a promise that resolves when the stream reads EOF. Used by Socket to detect the
  // remote end closing the connection. Must be called at most once per stream. Precondition:
  // !isNull().
  jsg::Promise<void> onEof(jsg::Lock& js);

  // Fully consume the stream, buffering up to `limit` bytes, and return all of its data as a single
  // value. Rejects if the stream has already been disturbed or would exceed `limit`; the stream is
  // left locked/consumed. A null stream yields an empty result.
  //
  // bytes()/json()/blob() are provided directly (rather than being layered on arrayBuffer()/text()
  // by the caller) to avoid an extra jsg::Promise allocation and microtask hop per call. blob()
  // takes the desired Content-Type (derived by the caller, e.g. from headers) because
  // JsReadableStream has no notion of headers.
  jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> arrayBuffer(jsg::Lock& js, uint64_t limit);
  jsg::Promise<kj::String> text(jsg::Lock& js, uint64_t limit);
  jsg::Promise<jsg::JsRef<jsg::JsUint8Array>> bytes(jsg::Lock& js, uint64_t limit);
  jsg::Promise<jsg::JsRef<jsg::JsValue>> json(jsg::Lock& js, uint64_t limit);
  jsg::Promise<jsg::Ref<Blob>> blob(jsg::Lock& js, uint64_t limit, kj::String contentType);

  // Fully drains the stream into the given sink. If end is EndStream::YES, the sink is closed after
  // the stream is fully drained. Deferred proxying is supported transparently if the underlying
  // source supports it. Precondition: !isNull().
  kj::Promise<DeferredProxy<void>> pumpTo(
      jsg::Lock& js, kj::Own<WritableStreamSink> sink, EndStream end);

  // Pipe this stream into the given destination, exactly like ReadableStream.prototype.pipeTo():
  // both ends are locked for the duration of the pipe, and the returned promise settles when the
  // pipe completes. Rejects (does not throw) if either end is already locked. By default the
  // destination is closed when this stream ends and aborted if it errors; see PipeToOptions.
  //
  // Both the source and destination are self-retained for the duration of the pipe; the caller
  // does not need to keep either alive after calling pipeTo(). The destination is borrowed, not
  // consumed: the caller's handle remains valid and observes the stream's state as the pipe
  // progresses. Unlike pumpTo(), this is a spec-level, isolate-bound pipe with no deferred-proxy
  // support; when both ends are backed by the C++ implementation the pipe still uses the
  // controllers' internal native fast paths. Preconditions: !isNull(), !destination.isNull().
  jsg::Promise<void> pipeTo(
      jsg::Lock& js, JsWritableStream& destination, PipeToOptions options = {});

  // Pipe this stream through the given transform, exactly like
  // ReadableStream.prototype.pipeThrough(): pipes this into transform.writable (with the pipe
  // promise marked as handled) and returns transform.readable. Throws synchronously if this
  // stream or transform.writable is already locked. The transform is consumed. Preconditions:
  // !isNull(), neither transform member is null.
  JsReadableStream pipeThrough(
      jsg::Lock& js, JsReadableWritablePair transform, PipeToOptions options = {});

  // Split a live stream into two independent branches that each read the same data, consuming
  // (nullifying) this. Both branches share the retransmit buffer (if any). Works for any stream.
  // Precondition: !isNull().
  Tee tee(jsg::Lock& js);

  // If this is buffer-backed, return a fresh, independent JsReadableStream that reads from the
  // start of the buffer (carrying the buffer forward, so the result is itself rewindable). This is
  // non-mutating -- `this` is left untouched. Returns kj::none if this is null or not
  // buffer-backed.
  kj::Maybe<JsReadableStream> tryClone(jsg::Lock& js);

  // Take over the internal stream state (and a reference to the retransmit buffer, if any) into a
  // new JsReadableStream, leaving `this` locked/disturbed. Precondition: !isNull().
  //
  // Detaching an already-disturbed stream is an error unless IgnoreDisturbed::YES is passed
  // (used when neutralizing a stream whose underlying data source is being taken over, e.g.
  // when a Socket's connection is adopted by another consumer).
  JsReadableStream detach(jsg::Lock& js, IgnoreDisturbed ignoreDisturbed = IgnoreDisturbed::NO);

  // Convert this into a null / empty stream.
  void nullify();

  void visitForGc(jsg::GcVisitor& visitor);
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

  // Describe this type to RTTI (and therefore to generated TypeScript) exactly as a
  // ReadableStream. See the delegated-RTTI support in jsg/rtti.h.
  using JsgRttiDelegate = jsg::Ref<ReadableStream>;

  static v8::Local<v8::Value> jsgWrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      JsReadableStream stream) {
    // Wrapping a null JsReadableStream indicates a bug: APIs that can produce "no stream"
    // express that as kj::Maybe<JsReadableStream> so that absence maps to JS null/undefined
    // rather than to a fabricated stream.
    auto& impl = KJ_ASSERT_NONNULL(stream.impl, "cannot wrap a null JsReadableStream");
    KJ_SWITCH_ONEOF(impl.stream) {
      KJ_CASE_ONEOF(legacy, jsg::Ref<ReadableStream>) {
        return typeWrapper.wrap(js, context, creator, kj::mv(legacy));
      }
      KJ_CASE_ONEOF(ts, jsg::JsRef<jsg::JsObject>) {
        KJ_UNIMPLEMENTED("TypeScript ReadableStream not yet supported");
      }
    }
    KJ_UNREACHABLE;
  }

  static kj::Maybe<JsReadableStream> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    // For now, we only support unwrapping the legacy C++ ReadableStream.
    // Later we will also support the TypeScript implementation.
    return typeWrapper.tryUnwrap(
        js, context, handle, static_cast<jsg::Ref<ReadableStream>*>(nullptr), parentObject);
  }

 private:
  explicit JsReadableStream(Impl impl): impl(kj::mv(impl)) {}

  // Build a buffer-backed Impl: wraps the buffer's bytes in an in-memory ReadableStream (which does
  // NOT support deferred proxying, since the bytes may have V8 heap provenance) and retains the
  // buffer for retransmission.
  static Impl bufferBackedImpl(jsg::Lock& js, kj::Rc<Buffer> buffer);

  kj::Maybe<Impl> impl;
};

struct JsReadableStream::Tee {
  JsReadableStream branch1;
  JsReadableStream branch2;
};

}  // namespace workerd::api
