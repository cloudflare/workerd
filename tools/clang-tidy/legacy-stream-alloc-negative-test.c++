// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Negative fixtures for workerd-legacy-stream-alloc: none of these may produce
// a diagnostic.

namespace workerd::jsg {

template <typename T>
class Ref {
 public:
  explicit Ref(T* ptr): ptr(ptr) {}

 private:
  T* ptr;
};

class Lock {
 public:
  template <typename T, typename... Params>
  Ref<T> alloc(Params&&... params) {
    return Ref<T>(new T(static_cast<Params&&>(params)...));
  }

  template <typename T, typename... Params>
  Ref<T> allocAccounted(unsigned long accountedSize, Params&&... params) {
    return Ref<T>(new T(static_cast<Params&&>(params)...));
  }
};

template <typename T, typename... Params>
Ref<T> alloc(Params&&... params) {
  return Ref<T>(new T(static_cast<Params&&>(params)...));
}

}  // namespace workerd::jsg

namespace workerd::api {

namespace jsg = workerd::jsg;

class ReadableStreamSource {};
class WritableStreamSink {};

class ReadableStream {
 public:
  explicit ReadableStream(ReadableStreamSource* source) {}
};

class WritableStream {
 public:
  explicit WritableStream(WritableStreamSink* sink) {}
};

// Types whose names merely contain "ReadableStream" / "WritableStream".
class ReadableStreamDefaultReader {
 public:
  explicit ReadableStreamDefaultReader(ReadableStreamSource* source) {}
};

class WritableStreamDefaultWriter {
 public:
  explicit WritableStreamDefaultWriter(WritableStreamSink* sink) {}
};

template <typename... T>
void use(T&&...) {}

class TransformStream {
 public:
  TransformStream(jsg::Ref<ReadableStream> readable, jsg::Ref<WritableStream> writable) {}
};

class JsReadableStream {
 public:
  explicit JsReadableStream(jsg::Ref<ReadableStream> stream) {}
  static JsReadableStream create(jsg::Lock& js, ReadableStreamSource* source);
};

class JsWritableStream {
 public:
  explicit JsWritableStream(jsg::Ref<WritableStream> stream) {}
  static JsWritableStream create(jsg::Lock& js, WritableStreamSink* sink);
};

// Case N1: the dispatch point itself may allocate the legacy stream.
JsReadableStream JsReadableStream::create(jsg::Lock& js, ReadableStreamSource* source) {
  // ... including through a lambda defined inside it.
  auto legacy = [&]() { return js.alloc<ReadableStream>(source); };
  if (source == nullptr) {
    return JsReadableStream(legacy());
  }
  return JsReadableStream(js.allocAccounted<ReadableStream>(sizeof(ReadableStream), source));
}

// Case N2: same for the writable dispatch point.
JsWritableStream JsWritableStream::create(jsg::Lock& js, WritableStreamSink* sink) {
  return JsWritableStream(js.alloc<WritableStream>(sink));
}

// Case N3: the recommended way to obtain a stream from C++.
JsReadableStream negativeCreate(jsg::Lock& js, ReadableStreamSource* source) {
  return JsReadableStream::create(js, source);
}

// Case N4: allocating unrelated JSG types, including ones whose names contain
// the stream type names, is fine.
void negativeOtherTypes(jsg::Lock& js, ReadableStreamSource* source, WritableStreamSink* sink) {
  use(js.alloc<ReadableStreamDefaultReader>(source),
      js.allocAccounted<WritableStreamDefaultWriter>(sizeof(WritableStreamDefaultWriter), sink),
      jsg::alloc<ReadableStreamDefaultReader>(source));
}

// Case N5: a same-named type in a different namespace is not the legacy stream.
namespace other {
class ReadableStream {};
class WritableStream {};
}  // namespace other

void negativeOtherNamespace(jsg::Lock& js) {
  use(js.alloc<other::ReadableStream>(), jsg::alloc<other::WritableStream>());
}

// Case N6: the legacy implementation's own internals suppress the check
// explicitly, both per line and per block.
jsg::Ref<ReadableStream> negativeSuppressedLine(jsg::Lock& js, ReadableStreamSource* source) {
  // The legacy stream's tee() hands out legacy branches by construction.
  return js.alloc<ReadableStream>(source);  // NOLINT(workerd-legacy-stream-alloc)
}

// NOLINTBEGIN(workerd-legacy-stream-alloc)
jsg::Ref<TransformStream> negativeSuppressedBlock(
    jsg::Lock& js, ReadableStreamSource* source, WritableStreamSink* sink) {
  return js.alloc<TransformStream>(
      js.alloc<ReadableStream>(source), js.alloc<WritableStream>(sink));
}
// NOLINTEND(workerd-legacy-stream-alloc)

}  // namespace workerd::api
