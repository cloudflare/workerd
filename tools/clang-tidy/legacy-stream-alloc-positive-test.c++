// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Positive fixtures for workerd-legacy-stream-alloc: each case must produce
// exactly one diagnostic; legacy-stream-alloc-test.sh asserts the exact total.

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

class JsReadableStream {
 public:
  static JsReadableStream create(jsg::Lock& js, ReadableStreamSource* source);

  // A member of the abstraction that is not the dispatch point.
  static jsg::Ref<ReadableStream> other(jsg::Lock& js, ReadableStreamSource* otherSource);
};

class JsWritableStream {
 public:
  static JsWritableStream create(jsg::Lock& js, WritableStreamSink* sink);
};

// Case P1: js.alloc<ReadableStream>() in a free function.
jsg::Ref<ReadableStream> positiveReadable(jsg::Lock& js, ReadableStreamSource* source) {
  return js.alloc<ReadableStream>(source);
}

// Case P2: js.alloc<WritableStream>() in a free function.
jsg::Ref<WritableStream> positiveWritable(jsg::Lock& js, WritableStreamSink* sink) {
  return js.alloc<WritableStream>(sink);
}

// Case P3: allocAccounted<ReadableStream>() is an allocation too.
jsg::Ref<ReadableStream> positiveAccounted(jsg::Lock& js, ReadableStreamSource* source) {
  return js.allocAccounted<ReadableStream>(sizeof(ReadableStream), source);
}

// Case P4: the free-function jsg::alloc<WritableStream>() form.
jsg::Ref<WritableStream> positiveFreeFunction(WritableStreamSink* sink) {
  return jsg::alloc<WritableStream>(sink);
}

// Case P5: the type spelled with namespace qualification.
jsg::Ref<ReadableStream> positiveQualified(jsg::Lock& js, ReadableStreamSource* source) {
  return js.alloc<::workerd::api::ReadableStream>(source);
}

// Case P6: inside a lambda defined in a function that is not a dispatch point.
jsg::Ref<ReadableStream> positiveLambda(jsg::Lock& js, ReadableStreamSource* source) {
  auto make = [&]() { return js.alloc<ReadableStream>(source); };
  return make();
}

// Case P7: a member of JsReadableStream other than create() is not exempt.
jsg::Ref<ReadableStream> JsReadableStream::other(jsg::Lock& js, ReadableStreamSource* otherSource) {
  return js.alloc<ReadableStream>(otherSource);
}

// Case P8: a function named create() on some other class is not exempt.
class Unrelated {
 public:
  static jsg::Ref<WritableStream> create(jsg::Lock& js, WritableStreamSink* unrelatedSink) {
    return js.alloc<WritableStream>(unrelatedSink);
  }
};

// Case P9: a generic helper is reported when instantiated with a legacy stream
// type. The diagnostic lands on the js.alloc<T>() in the template body.
template <typename T, typename Arg>
jsg::Ref<T> makeStream(jsg::Lock& js, Arg* arg) {
  return js.alloc<T>(arg);
}

jsg::Ref<ReadableStream> positiveGeneric(jsg::Lock& js, ReadableStreamSource* source) {
  return makeStream<ReadableStream>(js, source);
}

}  // namespace workerd::api
