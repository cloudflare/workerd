// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "common.h"
#include <kj/function.h>

namespace workerd::api {

class ReadableStreamDefaultReader;
class ReadableStreamBYOBReader;

class ReaderImpl final {
public:
  ReaderImpl(ReadableStreamController::Reader& reader);

  ~ReaderImpl() noexcept(false);

  void attach(ReadableStreamController& controller, jsg::Promise<void> closedPromise);

  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason);

  void detach();

  jsg::MemoizedIdentity<jsg::Promise<void>>& getClosed();

  void lockToStream(jsg::Lock& js, ReadableStream& stream);

  jsg::Promise<ReadResult> read(jsg::Lock& js,
                                 kj::Maybe<ReadableStreamController::ByobOptions> byobOptions);

  void releaseLock(jsg::Lock& js);

  void visitForGc(jsg::GcVisitor& visitor);

  kj::StringPtr jsgGetMemoryName() const;
  size_t jsgGetMemorySelfSize() const;
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  struct Initial {};
  // While a Reader is attached to a ReadableStream, it holds a strong reference to the
  // ReadableStream to prevent it from being GC'ed so long as the Reader is available.
  // Once the reader is closed, released, or GC'ed the reference to the ReadableStream
  // is cleared and the ReadableStream can be GC'ed if there are no other references to
  // it being held anywhere. If the reader is still attached to the ReadableStream when
  // it is destroyed, the ReadableStream's reference to the reader is cleared but the
  // ReadableStream remains in the "reader locked" state, per the spec.
  using Attached = jsg::Ref<ReadableStream>;
  struct Released {};

  kj::Maybe<IoContext&> ioContext;
  ReadableStreamController::Reader& reader;

  kj::OneOf<Initial, Attached, StreamStates::Closed, Released> state = Initial();
  kj::Maybe<jsg::MemoizedIdentity<jsg::Promise<void>>> closedPromise;

  friend class ReadableStreamDefaultReader;
  friend class ReadableStreamBYOBReader;
};

class ReadableStreamDefaultReader : public jsg::Object,
                                    public ReadableStreamController::Reader {
public:
  explicit ReadableStreamDefaultReader();

  // JavaScript API

  static jsg::Ref<ReadableStreamDefaultReader> constructor(
      jsg::Lock& js, jsg::Ref<ReadableStream> stream);

  jsg::MemoizedIdentity<jsg::Promise<void>>& getClosed();
  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason);
  jsg::Promise<ReadResult> read(jsg::Lock& js);
  void releaseLock(jsg::Lock& js);

  JSG_RESOURCE_TYPE(ReadableStreamDefaultReader, CompatibilityFlags::Reader flags) {
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(closed, getClosed);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(closed, getClosed);
    }
    JSG_METHOD(cancel);
    JSG_METHOD(read);
    JSG_METHOD(releaseLock);

    JSG_TS_OVERRIDE(<R = any> {
      read(): Promise<ReadableStreamReadResult<R>>;
    });
  }

  // Internal API

  void attach(ReadableStreamController& controller, jsg::Promise<void> closedPromise) override;

  void detach() override;

  void lockToStream(jsg::Lock& js, ReadableStream& stream);

  inline bool isByteOriented() const override { return false; }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("impl", impl);
  }

private:
  ReaderImpl impl;

  void visitForGc(jsg::GcVisitor& visitor);
};

class ReadableStreamBYOBReader: public jsg::Object,
                                public ReadableStreamController::Reader {
public:
  explicit ReadableStreamBYOBReader();

  // JavaScript API

  static jsg::Ref<ReadableStreamBYOBReader> constructor(
      jsg::Lock& js,
      jsg::Ref<ReadableStream> stream);

  jsg::MemoizedIdentity<jsg::Promise<void>>& getClosed();
  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason);

  struct ReadableStreamBYOBReaderReadOptions {
    jsg::Optional<int> min;
    JSG_STRUCT(min);
  };

  jsg::Promise<ReadResult> read(jsg::Lock& js, v8::Local<v8::ArrayBufferView> byobBuffer,
      jsg::Optional<ReadableStreamBYOBReaderReadOptions> options = kj::none);

  // Non-standard extension so that reads can specify a minimum number of bytes to read. It's a
  // struct so that we could eventually add things like timeouts if we need to. Since there's no
  // existing spec that's a leading contender, this is behind a different method name to avoid
  // conflicts with any changes to `read`. Fewer than `minBytes` may be returned if EOF is hit or
  // the underlying stream is closed/errors out. In all cases the read result is either
  // {value: theChunk, done: false} or {value: undefined, done: true} as with read.
  // TODO(soon): Like fetch() and Cache.match(), readAtLeast() returns a promise for a V8 object.
  jsg::Promise<ReadResult> readAtLeast(jsg::Lock& js,
                                        int minBytes,
                                        v8::Local<v8::ArrayBufferView> byobBuffer);

  void releaseLock(jsg::Lock& js);

  JSG_RESOURCE_TYPE(ReadableStreamBYOBReader, CompatibilityFlags::Reader flags) {
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(closed, getClosed);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(closed, getClosed);
    }
    JSG_METHOD(cancel);
    JSG_METHOD(read);
    JSG_METHOD(releaseLock);

    // Non-standard extension that should only apply to BYOB byte streams.
    JSG_METHOD(readAtLeast);

    JSG_TS_OVERRIDE(ReadableStreamBYOBReader {
      read<T extends ArrayBufferView>(view: T): Promise<ReadableStreamReadResult<T>>;
      readAtLeast<T extends ArrayBufferView>(minElements: number, view: T): Promise<ReadableStreamReadResult<T>>;
    });
  }

  // Internal API

  void attach(
      ReadableStreamController& controller,
      jsg::Promise<void> closedPromise) override;

  void detach() override;

  void lockToStream(jsg::Lock& js, ReadableStream& stream);

  inline bool isByteOriented() const override { return true; }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("impl", impl);
  }

private:
  ReaderImpl impl;

  void visitForGc(jsg::GcVisitor& visitor);
};

class ReadableStream: public jsg::Object {
private:

  struct AsyncIteratorState {
    kj::Maybe<IoContext&> ioContext;
    jsg::Ref<ReadableStreamDefaultReader> reader;
    bool preventCancel;
  };

  static jsg::Promise<kj::Maybe<jsg::Value>> nextFunction(
      jsg::Lock& js,
      AsyncIteratorState& state);

  static jsg::Promise<void> returnFunction(
      jsg::Lock& js,
      AsyncIteratorState& state,
      jsg::Optional<jsg::Value> value);

public:
  explicit ReadableStream(IoContext& ioContext,
                          kj::Own<ReadableStreamSource> source);

  explicit ReadableStream(kj::Own<ReadableStreamController> controller);

  ReadableStreamController& getController();

  jsg::Ref<ReadableStream> addRef();

  bool isDisturbed();

  // ---------------------------------------------------------------------------
  // JS interface

  // Creates a new JS-backed ReadableStream using the provided source and strategy.
  // We use v8::Local<v8::Object>'s here instead of jsg structs because we need
  // to preserve the object references within the implementation.
  static jsg::Ref<ReadableStream> constructor(
      jsg::Lock& js,
      jsg::Optional<UnderlyingSource> underlyingSource,
      jsg::Optional<StreamQueuingStrategy> queuingStrategy);

  static jsg::Ref<ReadableStream> from(jsg::Lock& js, jsg::AsyncGenerator<jsg::Value> generator);

  bool isLocked();

  // Closes the stream. All present and future read requests are fulfilled with successful empty
  // results. `reason` will be passed to the underlying source's cancel algorithm -- if this
  // readable stream is one side of a transform stream, then its cancel algorithm causes the
  // transform's writable side to become errored with `reason`.
  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason);

  using Reader = kj::OneOf<jsg::Ref<ReadableStreamDefaultReader>,
                           jsg::Ref<ReadableStreamBYOBReader>>;

  struct GetReaderOptions {
    jsg::Optional<kj::String> mode;  // can be "byob" or undefined

    JSG_STRUCT(mode);

    JSG_STRUCT_TS_OVERRIDE({ mode: "byob" });
    // Intentionally required, so we can use `GetReaderOptions` directly in the
    // `ReadableStream#getReader()` overload.
  };

  Reader getReader(jsg::Lock& js, jsg::Optional<GetReaderOptions> options);

  // Options specifically for the values() function.
  struct ValuesOptions {
    jsg::Optional<bool> preventCancel = false;
    JSG_STRUCT(preventCancel);
  };

  JSG_ASYNC_ITERATOR_WITH_OPTIONS(ReadableStreamAsyncIterator,
                                   values,
                                   jsg::Value,
                                   AsyncIteratorState,
                                   nextFunction,
                                   returnFunction,
                                   ValuesOptions);
  struct Transform {
    jsg::Ref<WritableStream> writable;
    jsg::Ref<ReadableStream> readable;

    JSG_STRUCT(writable, readable);
    JSG_STRUCT_TS_OVERRIDE(ReadableWritablePair<R = any, W = any> {
      readable: ReadableStream<R>;
      writable: WritableStream<W>;
    });
  };

  jsg::Ref<ReadableStream> pipeThrough(
      jsg::Lock& js,
      Transform transform,
      jsg::Optional<PipeToOptions> options);

  jsg::Promise<void> pipeTo(
      jsg::Lock& js,
      jsg::Ref<WritableStream> destination,
      jsg::Optional<PipeToOptions> options);

  // Locks the stream and returns a pair of two new ReadableStreams, each of which read the same
  // data as this ReadableStream would.
  kj::Array<jsg::Ref<ReadableStream>> tee(jsg::Lock& js);

  jsg::JsString inspectState(jsg::Lock& js);
  bool inspectSupportsBYOB();
  jsg::Optional<uint64_t> inspectLength();

  JSG_RESOURCE_TYPE(ReadableStream, CompatibilityFlags::Reader flags) {
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(locked, isLocked);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(locked, isLocked);
    }
    JSG_METHOD(cancel);
    JSG_METHOD(getReader);
    JSG_METHOD(pipeThrough);
    JSG_METHOD(pipeTo);
    JSG_METHOD(tee);
    JSG_METHOD(values);
    JSG_STATIC_METHOD(from);

    JSG_INSPECT_PROPERTY(state, inspectState);
    JSG_INSPECT_PROPERTY(supportsBYOB, inspectSupportsBYOB);
    JSG_INSPECT_PROPERTY(length, inspectLength);

    JSG_ASYNC_ITERABLE(values);

    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_TS_DEFINE(interface ReadableStream<R = any> {
        get locked(): boolean;

        cancel(reason?: any): Promise<void>;

        getReader(): ReadableStreamDefaultReader<R>;
        getReader(options: ReadableStreamGetReaderOptions): ReadableStreamBYOBReader;

        pipeThrough<T>(transform: ReadableWritablePair<T, R>, options?: StreamPipeOptions): ReadableStream<T>;
        pipeTo(destination: WritableStream<R>, options?: StreamPipeOptions): Promise<void>;

        tee(): [ReadableStream<R>, ReadableStream<R>];

        values(options?: ReadableStreamValuesOptions): AsyncIterableIterator<R>;
        [Symbol.asyncIterator](options?: ReadableStreamValuesOptions): AsyncIterableIterator<R>;
      });
    } else {
      JSG_TS_DEFINE(interface ReadableStream<R = any> {
        readonly locked: boolean;

        cancel(reason?: any): Promise<void>;

        getReader(): ReadableStreamDefaultReader<R>;
        getReader(options: ReadableStreamGetReaderOptions): ReadableStreamBYOBReader;

        pipeThrough<T>(transform: ReadableWritablePair<T, R>, options?: StreamPipeOptions): ReadableStream<T>;
        pipeTo(destination: WritableStream<R>, options?: StreamPipeOptions): Promise<void>;

        tee(): [ReadableStream<R>, ReadableStream<R>];

        values(options?: ReadableStreamValuesOptions): AsyncIterableIterator<R>;
        [Symbol.asyncIterator](options?: ReadableStreamValuesOptions): AsyncIterableIterator<R>;
      });
    }
    // Replace ReadableStream class with an interface and const, so we can have
    // two constructors with differing type parameters for byte-oriented and
    // value-oriented streams.
    JSG_TS_OVERRIDE(const ReadableStream: {
      prototype: ReadableStream;
      new (underlyingSource: UnderlyingByteSource, strategy?: QueuingStrategy<Uint8Array>): ReadableStream<Uint8Array>;
      new <R = any>(underlyingSource?: UnderlyingSource<R>, strategy?: QueuingStrategy<R>): ReadableStream<R>;
    });
  }

  // Detaches this ReadableStream from its underlying controller state, returning a
  // new ReadableStream instance that takes over the underlying state. This is used to
  // support the "create a proxy" of a ReadableStream algorithm in the streams spec
  // (see https://streams.spec.whatwg.org/#readablestream-create-a-proxy). In that
  // algorithm, it says to create a proxy of a stream by creating a new TransformStream
  // and piping the original through it. The readable side of the created transform
  // becomes the proxy. That is quite inefficient so instead, we create a new
  // ReadableStream that will take over ownership of the internal state of this one,
  // leaving this ReadableStream locked and disturbed so that it is no longer usable.
  // The name "detach" here is used in the sense of "detaching the internal state".
  jsg::Ref<ReadableStream> detach(jsg::Lock& js, bool ignoreDisturbed=false);

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding);

  // A potentially optimized version of pipe that sends this stream's data to the given
  // sink. The entire stream is consumed. The ReadableStream will be left locked and
  // disturbed and the DeferredProxy returned will take over ownership of the internal
  // state of the readable.
  kj::Promise<DeferredProxy<void>> pumpTo(jsg::Lock& js,
                                          kj::Own<WritableStreamSink> sink,
                                          bool end);

  // Initializes signalling mechanism for EOF detection. Returns a promise that will resolve when
  // EOF is reached.
  //
  // This method should only be called once.
  jsg::Promise<void> onEof(jsg::Lock& js);

  // Used by ReadableStreamInternalController to signal EOF being reached. Can be called even if
  // `onEof` wasn't called.
  void signalEof(jsg::Lock& js);

  void serialize(jsg::Lock& js, jsg::Serializer& serializer);
  static jsg::Ref<ReadableStream> deserialize(
      jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer);

  JSG_SERIALIZABLE(rpc::SerializationTag::READABLE_STREAM);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  kj::Maybe<IoContext&> ioContext;
  kj::Own<ReadableStreamController> controller;

  // Used to signal when this ReadableStream reads EOF. This signal is required for TCP sockets.
  kj::Maybe<jsg::PromiseResolverPair<void>> eofResolverPair;

  void visitForGc(jsg::GcVisitor& visitor);
};

struct QueuingStrategyInit {
  double highWaterMark;
  JSG_STRUCT(highWaterMark);
};

using QueuingStrategySizeFunction =
    jsg::Optional<uint32_t>(jsg::Optional<v8::Local<v8::Value>>);

// Utility class defined by the streams spec that uses byteLength to calculate
// backpressure changes.
class ByteLengthQueuingStrategy: public jsg::Object {
public:
  ByteLengthQueuingStrategy(QueuingStrategyInit init) : init(init) {}

  static jsg::Ref<ByteLengthQueuingStrategy> constructor(QueuingStrategyInit init) {
    return jsg::alloc<ByteLengthQueuingStrategy>(init);
  }

  double getHighWaterMark() const { return init.highWaterMark; }

  jsg::Function<QueuingStrategySizeFunction> getSize() const { return &size; }

  JSG_RESOURCE_TYPE(ByteLengthQueuingStrategy) {
    JSG_READONLY_PROTOTYPE_PROPERTY(highWaterMark, getHighWaterMark);
    JSG_READONLY_PROTOTYPE_PROPERTY(size, getSize);

    // QueuingStrategy requires the result of the size function to be defined
    JSG_TS_OVERRIDE(implements QueuingStrategy<ArrayBufferView> {
      get size(): (chunk?: any) => number;
    });
  }

private:
  static jsg::Optional<uint32_t> size(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>>);

  QueuingStrategyInit init;
};

// Utility class defined by the streams spec that uses a fixed value of 1 to calculate
// backpressure change
class CountQueuingStrategy: public jsg::Object {
public:
  CountQueuingStrategy(QueuingStrategyInit init) : init(init) {}

  static jsg::Ref<CountQueuingStrategy> constructor(QueuingStrategyInit init) {
    return jsg::alloc<CountQueuingStrategy>(init);
  }

  double getHighWaterMark() const { return init.highWaterMark; }

  jsg::Function<QueuingStrategySizeFunction> getSize() const { return &size; }

  JSG_RESOURCE_TYPE(CountQueuingStrategy) {
    JSG_READONLY_PROTOTYPE_PROPERTY(highWaterMark, getHighWaterMark);
    JSG_READONLY_PROTOTYPE_PROPERTY(size, getSize);

    // QueuingStrategy requires the result of the size function to be defined
    JSG_TS_OVERRIDE(implements QueuingStrategy {
      get size(): (chunk?: any) => number;
    });
  }

private:
  static jsg::Optional<uint32_t> size(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>>) {
    return 1;
  }

  QueuingStrategyInit init;
};

}  // namespace workerd::api
