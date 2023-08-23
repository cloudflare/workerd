// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <kj/vector.h>

namespace workerd::jsg {

class Serializer final: v8::ValueSerializer::Delegate {
  // Wraps the v8::ValueSerializer and v8::ValueSerializer::Delegate implementation.
public:
  struct Options {
    kj::Maybe<uint32_t> version;
    // When set, overrides the default wire format version with the one provided.
    bool omitHeader = false;
    // When set to true, the serialization header is not written to the output buffer.
  };

  struct Released {
    kj::Array<kj::byte> data;
    kj::Array<std::shared_ptr<v8::BackingStore>> sharedArrayBuffers;
    kj::Array<std::shared_ptr<v8::BackingStore>> transferedArrayBuffers;
  };

  explicit Serializer(Lock& js, kj::Maybe<Options> maybeOptions = nullptr);
  inline ~Serializer() noexcept(true) {}  // noexcept(true) because Delegate's is noexcept

  KJ_DISALLOW_COPY_AND_MOVE(Serializer);

  void write(Lock& js, v8::Local<v8::Value> value);

  inline void write(Lock& js, Value value) { write(js, value.getHandle(js)); }

  template <typename T>
  inline void write(Lock& js, V8Ref<T> value) { write(js, value.getHandle(js)); }

  void transfer(Lock& js, v8::Local<v8::ArrayBuffer> arrayBuffer);

  Released release();

private:
  // v8::ValueSerializer::Delegate implementation
  void ThrowDataCloneError(v8::Local<v8::String> message) override;

  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate* isolate,
      v8::Local<v8::SharedArrayBuffer> sab) override;

  kj::Vector<V8Ref<v8::SharedArrayBuffer>> sharedArrayBuffers;
  kj::Vector<V8Ref<v8::ArrayBuffer>> arrayBuffers;
  kj::Vector<std::shared_ptr<v8::BackingStore>> sharedBackingStores;
  kj::Vector<std::shared_ptr<v8::BackingStore>> backingStores;
  v8::ValueSerializer ser;
  bool released = false;
};

class Deserializer final: v8::ValueDeserializer::Delegate {
public:
  struct Options {
    kj::Maybe<uint32_t> version;
    bool readHeader = true;
  };

  explicit Deserializer(
      Lock& js,
      kj::ArrayPtr<const kj::byte> data,
      kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferedArrayBuffers = nullptr,
      kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> sharedArrayBuffers = nullptr,
      kj::Maybe<Options> maybeOptions = nullptr);

  explicit Deserializer(
      Lock& js,
      Serializer::Released& released,
      kj::Maybe<Options> maybeOptions = nullptr);

  ~Deserializer() noexcept(true) {}  // noexcept(true) because Delegate's is noexcept

  KJ_DISALLOW_COPY_AND_MOVE(Deserializer);

  v8::Local<v8::Value> readValue(Lock& js);

  inline uint32_t getVersion() const { return deser.GetWireFormatVersion(); }

private:
  void init(
      Lock& js,
      kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferedArrayBuffers = nullptr,
      kj::Maybe<Options> maybeOptions = nullptr);

  v8::MaybeLocal<v8::SharedArrayBuffer> GetSharedArrayBufferFromId(
      v8::Isolate* isolate,
      uint32_t clone_id) override;

  v8::ValueDeserializer deser;
  kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> sharedBackingStores;
};

class SerializedBufferDisposer: public kj::ArrayDisposer {
  // Intended for use with v8::ValueSerializer data released into a kj::Array.
protected:
  void disposeImpl(void* firstElement, size_t elementSize, size_t elementCount,
                   size_t capacity, void (*destroyElement)(void*)) const override;
};
constexpr SerializedBufferDisposer SERIALIZED_BUFFER_DISPOSER;

v8::Local<v8::Value> structuredClone(
    Lock& js,
    v8::Local<v8::Value> value,
    kj::Maybe<kj::ArrayPtr<jsg::Value>> maybeTransfer = nullptr);

}  // namespace workerd::jsg
