// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ser.h"

namespace workerd::jsg {

Serializer::Serializer(v8::Isolate* isolate, kj::Maybe<Options> maybeOptions)
    : isolate(isolate),
      ser(isolate, this) {
  auto options = maybeOptions.orDefault({});
  KJ_IF_MAYBE(version, options.version) {
    KJ_ASSERT(*version >= 13, "The minimum serialization version is 13.");
    KJ_ASSERT(jsg::check(ser.SetWriteVersion(*version)));
  }
  if (!options.omitHeader) {
    ser.WriteHeader();
  }
}

v8::Maybe<uint32_t> Serializer::GetSharedArrayBufferId(
    v8::Isolate *isolate,
    v8::Local<v8::SharedArrayBuffer> sab) {
  uint32_t n;
  for (n = 0; n < sharedArrayBuffers.size(); n++) {
    // If the SharedArrayBuffer has already been added, return the existing ID for it.
    if (sharedArrayBuffers[n].getHandle(isolate) == sab) {
      return v8::Just(n);
    }
  }
  sharedArrayBuffers.add(jsg::V8Ref(isolate, sab));
  sharedBackingStores.add(sab->GetBackingStore());
  return v8::Just(n);
}

void Serializer::ThrowDataCloneError(v8::Local<v8::String> message) {
  // This could throw an exception. If it does, we'll end up crashing but that's ok?
  isolate->ThrowException(makeDOMException(isolate, message, "DataCloneError"));
}

Serializer::Released Serializer::release() {
  KJ_ASSERT(!released, "The data has already been released.");
  released = true;
  sharedArrayBuffers.clear();
  arrayBuffers.clear();
  auto pair = ser.Release();
  return Released {
    .data = kj::Array(pair.first, pair.second, jsg::SERIALIZED_BUFFER_DISPOSER),
    .sharedArrayBuffers = sharedBackingStores.releaseAsArray(),
    .transferedArrayBuffers = backingStores.releaseAsArray(),
  };
}

void Serializer::transfer(v8::Local<v8::ArrayBuffer> arrayBuffer) {
  KJ_ASSERT(!released, "The data has already been released.");
  uint32_t n;
  for (n = 0; n < arrayBuffers.size(); n++) {
    // If the ArrayBuffer has already been added, we do not want to try adding it again.
    if (arrayBuffers[n].getHandle(isolate) == arrayBuffer) {
      return;
    }
  }

  arrayBuffers.add(jsg::V8Ref(isolate, arrayBuffer));
  backingStores.add(arrayBuffer->GetBackingStore());
  check(arrayBuffer->Detach(v8::Local<v8::Value>()));
  ser.TransferArrayBuffer(n, arrayBuffer);
}

void Serializer::write(v8::Local<v8::Value> value) {
  KJ_ASSERT(!released, "The data has already been released.");
  KJ_ASSERT(check(ser.WriteValue(isolate->GetCurrentContext(),value)));
}

void Deserializer::init(
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferedArrayBuffers,
    kj::Maybe<Options> maybeOptions) {
  auto options = maybeOptions.orDefault({});
  if (options.readHeader) {
    check(deser.ReadHeader(isolate->GetCurrentContext()));
  }
  KJ_IF_MAYBE(version, options.version) {
    KJ_ASSERT(*version >= 13, "The minimum serialization version is 13.");
    deser.SetWireFormatVersion(*version);
  }
  KJ_IF_MAYBE(arrayBuffers, transferedArrayBuffers) {
    for (auto n = 0; n < arrayBuffers->size(); n++) {
      deser.TransferArrayBuffer(n,
          v8::ArrayBuffer::New(isolate, kj::mv((*arrayBuffers)[n])));
    }
  }
}

v8::Local<v8::Value> Deserializer::readValue() {
  return check(deser.ReadValue(isolate->GetCurrentContext()));
}

v8::MaybeLocal<v8::SharedArrayBuffer> Deserializer::GetSharedArrayBufferFromId(
    v8::Isolate* isolate,
    uint32_t clone_id) {
  KJ_IF_MAYBE(backingStores, sharedBackingStores) {
    KJ_ASSERT(clone_id < backingStores->size());
    return v8::SharedArrayBuffer::New(isolate, (*backingStores)[clone_id]);
  }
  return v8::MaybeLocal<v8::SharedArrayBuffer>();
}

void SerializedBufferDisposer::disposeImpl(
    void* firstElement,
    size_t elementSize,
    size_t elementCount,
    size_t capacity,
    void (*destroyElement)(void*)) const {
  free(firstElement);
}

v8::Local<v8::Value> structuredClone(
    v8::Local<v8::Value> value,
    v8::Isolate* isolate,
    kj::Maybe<kj::ArrayPtr<jsg::Value>> maybeTransfer) {
  Serializer ser(isolate, nullptr);
  KJ_IF_MAYBE(transfers, maybeTransfer) {
    for (auto& item : *transfers) {
      auto val = item.getHandle(isolate);
      JSG_REQUIRE(val->IsArrayBuffer(), TypeError, "Object is not transferable");
      ser.transfer(val.As<v8::ArrayBuffer>());
    }
  }
  ser.write(value);
  auto released = ser.release();
  Deserializer des(isolate, released, nullptr);
  return des.readValue();
}

}  // namespace workerd::jsg
