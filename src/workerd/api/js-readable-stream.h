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
// When stream-backed, the underlying stream is either a legacy C++ ReadableStream or a TypeScript
// implemented ReadableStream (a JS object).
class JsReadableStream final {
 public:
  // The underlying stream.
  using StreamImpl = kj::OneOf<jsg::Ref<ReadableStream>, jsg::JsRef<jsg::JsObject>>;

  // Holds the in-memory data backing a buffer-backed (rewindable) JsReadableStream.
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

  // Adopt a TypeScript-implemented ReadableStream (a JS object). No brand validation
  // happens here -- the caller asserts the object really is one. Untrusted values arrive
  // through jsgTryUnwrap/tryUnwrapTs, which brand-check before adopting; internal callers
  // (create()) construct from a known-good conduit instance.
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
  // This is the compatibility-flag dispatch point: when the typescript_implemented_streams
  // compat flag is enabled, the source is wrapped in a ReadableStreamNativeSource and the
  // stream is constructed by the TypeScript implementation; otherwise the legacy C++
  // ReadableStream is used.
  //
  // TODO(streams-ts): A few JsReadableStream operations still have unimplemented
  // TypeScript arms (detach, pipe dispatch cells), so under the (experimental) flag,
  // consumers exercising those paths will fail until the remaining arms are
  // implemented. (pumpTo, unwrap, and tee have landed.)
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
  operator bool() const {
    return !isNull();
  }

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
        // The TypeScript-implemented stream IS a JS object; wrapping just hands the same
        // handle back, which is what preserves identity (request.body === request.body).
        return ts.getHandle(js);
      }
    }
    KJ_UNREACHABLE;
  }

  static kj::Maybe<JsReadableStream> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    KJ_IF_SOME(legacy,
        typeWrapper.tryUnwrap(
            js, context, handle, static_cast<jsg::Ref<ReadableStream>*>(nullptr), parentObject)) {
      return JsReadableStream(kj::mv(legacy));
    }
    // TypeScript-implemented streams are plain JS objects with no JSG wrapper, so the
    // typeWrapper cannot recognize them; ask the TS implementation's own brand check.
    return tryUnwrapTs(js, handle);
  }

  // The TypeScript arm of jsgTryUnwrap: recognizes a TypeScript-implemented ReadableStream
  // by the implementation's private brand (via the bootstrap bridge's isReadableStream) and
  // adopts it. Returns kj::none if the typescript_implemented_streams compat flag is off
  // (the bootstrap export does not exist then) or if the value is not a TS stream.
  //
  // Deliberately performs no locked/disturbed checks, matching the legacy arm: unwrap
  // adopts the handle as-is and consumers (e.g. Body) enforce their own preconditions.
  // Public so tests can drive it directly; production code goes through jsgTryUnwrap.
  static kj::Maybe<JsReadableStream> tryUnwrapTs(jsg::Lock& js, v8::Local<v8::Value> handle);

 private:
  explicit JsReadableStream(Impl impl): impl(kj::mv(impl)) {}

  // Build a buffer-backed Impl: wraps the buffer's bytes in an in-memory ReadableStream (which does
  // NOT support deferred proxying, since the bytes may have V8 heap provenance) and retains the
  // buffer for retransmission.
  static Impl bufferBackedImpl(jsg::Lock& js, kj::Rc<Buffer> buffer);

  kj::Maybe<Impl> impl;
  // Cache the result of isDisturbed() so that repeated calls don't dispatch to the JS
  // side. Since disturbed is a one-way transition, the cache is always valid once set
  // to true.
  bool cachedIsDisturbed = false;
};

struct JsReadableStream::Tee {
  JsReadableStream branch1;
  JsReadableStream branch2;
};

// The C++ implementation of the "native underlying source" contract defined by the
// TypeScript streams implementation.
//
// A ReadableStreamNativeSource wraps a kj-native ReadableStreamSource so that a
// TypeScript-implemented ReadableStream can be backed directly by C++ byte sources: the
// TypeScript conduit invokes pull()/cancel() (and eventually tee()) on this object, which
// translates them into tryRead()/cancel() calls on the underlying source.
//
// Instances are created only from C++ (there is no JavaScript constructor) and are handed
// to the TypeScript ReadableStream constructor carrying the native-source marker symbol.
// The object is never exposed to user code, and it is suppressed from the generated TypeScript
// types.
class ReadableStreamNativeSource final: public jsg::Object {
 public:
  ReadableStreamNativeSource(IoContext& ioContext, kj::Own<ReadableStreamSource> source);

  // The contract's data-delivery hook, called by the TypeScript conduit whenever there is
  // read demand, with at most one pull in flight at a time. `controller` is the conduit's
  // controller facade: its byobRequest property discriminates the read mode (an object for
  // BYOB reads, null for default reads), and delivery happens through exactly one
  // byobRequest.respond() / controller.enqueue() / controller.close() call per pull (pull
  // rejection is the error path). `signal` is the per-pull cancellation signal: it aborts
  // when the consumer abandons the in-flight read (reader release, cancel, tee), in which
  // case any bytes read are retained for redelivery by the next pull rather than delivered
  // or dropped.
  jsg::Promise<void> pull(jsg::Lock& js, jsg::JsObject controller, jsg::Ref<AbortSignal> signal);

  // Cancels the underlying source, indicating a loss of interest in the data, and releases
  // it. If a pull's read is in flight, the release is deferred until that read settles
  // (destroying a ReadableStreamSource with an outstanding tryRead() is unsafe); any bytes
  // that read produces are discarded.
  void cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason);

  // Splits the source into two fully independent branch sources, each of which will
  // produce the same bytes this source would have produced, leaving this source consumed.
  // Uses the underlying source's optimized tryTee() when available, and otherwise falls
  // back to a generic kj::newTee()-based split (mirroring the legacy internal controller's
  // tee). Any bytes retained from an abandoned pull are inherited by BOTH branches,
  // delivered before anything further from upstream.
  //
  // Throws if the source has already been consumed, or if an abandoned pull's read is
  // still in flight.
  kj::Array<jsg::Ref<ReadableStreamNativeSource>> tee(jsg::Lock& js);

  // The total number of bytes the source promises to produce, if known. Queried live from
  // the underlying source on each call; undefined once the source is done or canceled. The
  // TypeScript side reads this once at stream construction, per the contract, and enforces
  // the exact-total accounting itself.
  jsg::Optional<jsg::JsBigInt> getExpectedLength(jsg::Lock& js);

  JSG_RESOURCE_TYPE(ReadableStreamNativeSource) {
    JSG_PRIVATE_SYMBOL(kNativeSource);
    JSG_METHOD(pull);
    JSG_METHOD(cancel);
    JSG_METHOD(tee);
    JSG_READONLY_PROTOTYPE_PROPERTY(expectedLength, getExpectedLength);

    // Internal plumbing type: keep it out of the generated TypeScript types.
    JSG_TS_OVERRIDE(type ReadableStreamNativeSource = never);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  struct Active {
    IoOwn<ReadableStreamSource> source;
  };

  jsg::Promise<void> pullDefault(
      jsg::Lock& js, jsg::JsObject controller, jsg::Ref<AbortSignal> signal, Active& active);
  jsg::Promise<void> pullByob(jsg::Lock& js,
      jsg::JsObject controller,
      jsg::JsObject byobRequest,
      jsg::Ref<AbortSignal> signal,
      Active& active);

  struct Released {
    kj::Own<ReadableStreamSource> source;

    // Bytes already consumed from the source but never delivered (the stash), which the
    // pump must emit before anything the source produces. Only reachable when a
    // tee-seeded branch is extracted before being read.
    kj::Array<kj::byte> prefix;
  };

  // Releases the underlying source (plus any stashed bytes) for a C++-driven pump.
  // Called by JsReadableStream::pumpTo()'s TypeScript arm after the TypeScript-side
  // extractor (kExtractNativeSource) has already detached the stream, validated it
  // unlocked/undisturbed, and returned this object. If the source already completed (EOF
  // or cancel released it), returns an always-EOF source: extraction of closed streams is
  // legal per the contract, and the pump simply finishes.
  Released releaseForPump(jsg::Lock& js);

  // Ensure the scratch buffer can hold at least `capacity` bytes. Only called at the start
  // of a pull, when the scratch buffer holds no live data, so growing may discard previous
  // contents.
  void ensureScratch(size_t capacity);

  // Drop the first `bytes` bytes of the stash (they have been delivered).
  void consumeStash(size_t bytes);

  // kj::none once the source has reached EOF, been canceled, or been consumed by tee().
  kj::Maybe<Active> state;

  // Reusable read target, lazily allocated. KJ reads always land here -- never directly in
  // V8-visible memory -- and delivery copies out under the isolate lock. Sized
  // kScratchSize, growing when a BYOB read's minimum exceeds that (the conduit never
  // re-pulls an unsatisfied minimum, so a single read must be able to satisfy it).
  kj::Array<kj::byte> scratch;

  // Bytes that were read by a pull whose consumer abandoned it (per-pull signal aborted)
  // before delivery. Redelivered by the next pull, in order, before any new data, so no
  // bytes are lost across a reader release. Multiple abandoned pulls may accumulate.
  kj::Vector<kj::byte> stash;

  // Defensive only: the TypeScript conduit guarantees at most one pull in flight (standard
  // pulling/pullAgain serialization).
  bool pullInFlight = false;

  // Set when cancel() arrives while a pull's read is in flight: the source's release is
  // deferred to the pull's settlement.
  bool pendingCancel = false;

  static constexpr size_t kScratchSize = 32 * 1024;

  // JsReadableStream::pumpTo()'s TypeScript arm extracts the source for C++-driven pumps.
  friend class JsReadableStream;
};

}  // namespace workerd::api
