// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/buffersource.h>
#include <kj/vector.h>

namespace workerd::jsg {

// Wraps the v8::ValueSerializer and v8::ValueSerializer::Delegate implementation.
// Must be allocated on the stack, and requires that a v8::HandleScope exist in
// the stack.
class Serializer final: v8::ValueSerializer::Delegate {
public:
  struct Options {
    // When set, overrides the default wire format version with the one provided.
    kj::Maybe<uint32_t> version;
    // When set to true, the serialization header is not written to the output buffer.
    bool omitHeader = false;
  };

  struct Released {
    kj::Array<kj::byte> data;
    kj::Array<std::shared_ptr<v8::BackingStore>> sharedArrayBuffers;
    kj::Array<std::shared_ptr<v8::BackingStore>> transferredArrayBuffers;
  };

  explicit Serializer(Lock& js, kj::Maybe<Options> maybeOptions = kj::none);
  inline ~Serializer() noexcept(true) {}  // noexcept(true) because Delegate's is noexcept

  KJ_DISALLOW_COPY_AND_MOVE(Serializer);

  void write(Lock& js, const JsValue& value);

  void transfer(Lock& js, const JsValue& value);

  Released release();

private:
  // v8::ValueSerializer::Delegate implementation
  void ThrowDataCloneError(v8::Local<v8::String> message) override;

  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate* isolate,
      v8::Local<v8::SharedArrayBuffer> sab) override;

  kj::Vector<JsValue> sharedArrayBuffers;
  kj::Vector<JsValue> arrayBuffers;
  kj::Vector<std::shared_ptr<v8::BackingStore>> sharedBackingStores;
  kj::Vector<std::shared_ptr<v8::BackingStore>> backingStores;
  v8::ValueSerializer ser;
  bool released = false;
};

// Wraps the v8::ValueDeserializer and v8::ValueDeserializer::Delegate implementation.
// Must be allocated on the stack, and requires that a v8::HandleScope exist in
// the stack.
class Deserializer final: v8::ValueDeserializer::Delegate {
public:
  struct Options {
    kj::Maybe<uint32_t> version;
    bool readHeader = true;
  };

  explicit Deserializer(
      Lock& js,
      kj::ArrayPtr<const kj::byte> data,
      kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferredArrayBuffers = kj::none,
      kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> sharedArrayBuffers = kj::none,
      kj::Maybe<Options> maybeOptions = kj::none);

  explicit Deserializer(
      Lock& js,
      Serializer::Released& released,
      kj::Maybe<Options> maybeOptions = kj::none);

  ~Deserializer() noexcept(true) {}  // noexcept(true) because Delegate's is noexcept

  KJ_DISALLOW_COPY_AND_MOVE(Deserializer);

  JsValue readValue(Lock& js);

  inline uint32_t getVersion() const { return deser.GetWireFormatVersion(); }

private:
  void init(
      Lock& js,
      kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferredArrayBuffers = kj::none,
      kj::Maybe<Options> maybeOptions = kj::none);

  v8::MaybeLocal<v8::SharedArrayBuffer> GetSharedArrayBufferFromId(
      v8::Isolate* isolate,
      uint32_t clone_id) override;

  v8::ValueDeserializer deser;
  kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> sharedBackingStores;
};

// Intended for use with v8::ValueSerializer data released into a kj::Array.
class SerializedBufferDisposer: public kj::ArrayDisposer {
protected:
  void disposeImpl(void* firstElement, size_t elementSize, size_t elementCount,
                   size_t capacity, void (*destroyElement)(void*)) const override;
};
constexpr SerializedBufferDisposer SERIALIZED_BUFFER_DISPOSER;

JsValue structuredClone(
    Lock& js,
    const JsValue& value,
    kj::Maybe<kj::Array<JsValue>> maybeTransfer = kj::none);

}  // namespace workerd::jsg
