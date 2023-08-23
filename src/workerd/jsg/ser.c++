// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ser.h"

namespace workerd::jsg {

Serializer::Serializer(Lock& js, kj::Maybe<Options> maybeOptions)
    : ser(js.v8Isolate, this) {
#ifdef KJ_DEBUG
  kj::requireOnStack(this, "jsg::Serializer must be allocated on the stack");
#endif
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
  // makeDOMException could throw an exception. If it does, we'll end up crashing but that's ok?
  auto isolate = v8::Isolate::GetCurrent();
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

void Serializer::transfer(Lock& js, v8::Local<v8::ArrayBuffer> arrayBuffer) {
  KJ_ASSERT(!released, "The data has already been released.");
  uint32_t n;
  for (n = 0; n < arrayBuffers.size(); n++) {
    // If the ArrayBuffer has already been added, we do not want to try adding it again.
    if (arrayBuffers[n].getHandle(js) == arrayBuffer) {
      return;
    }
  }

  arrayBuffers.add(js.v8Ref(arrayBuffer));
  backingStores.add(arrayBuffer->GetBackingStore());
  check(arrayBuffer->Detach(v8::Local<v8::Value>()));
  ser.TransferArrayBuffer(n, arrayBuffer);
}

void Serializer::write(Lock& js, v8::Local<v8::Value> value) {
  KJ_ASSERT(!released, "The data has already been released.");
  KJ_ASSERT(check(ser.WriteValue(js.v8Context(), value)));
}

Deserializer::Deserializer(
    Lock& js,
    kj::ArrayPtr<const kj::byte> data,
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferedArrayBuffers,
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> sharedArrayBuffers,
    kj::Maybe<Options> maybeOptions)
    : deser(js.v8Isolate, data.begin(), data.size(), this),
      sharedBackingStores(kj::mv(sharedArrayBuffers)) {
#ifdef KJ_DEBUG
  kj::requireOnStack(this, "jsg::Deserializer must be allocated on the stack");
#endif
  init(js, kj::mv(transferedArrayBuffers), kj::mv(maybeOptions));
}

Deserializer::Deserializer(
    Lock& js,
    Serializer::Released& released,
    kj::Maybe<Options> maybeOptions)
    : Deserializer(
        js,
        released.data.asPtr(),
        released.transferedArrayBuffers.asPtr(),
        released.sharedArrayBuffers.asPtr(),
        kj::mv(maybeOptions)) {}

void Deserializer::init(
    Lock& js,
    kj::Maybe<kj::ArrayPtr<std::shared_ptr<v8::BackingStore>>> transferedArrayBuffers,
    kj::Maybe<Options> maybeOptions) {
  auto options = maybeOptions.orDefault({});
  if (options.readHeader) {
    check(deser.ReadHeader(js.v8Context()));
  }
  KJ_IF_MAYBE(version, options.version) {
    KJ_ASSERT(*version >= 13, "The minimum serialization version is 13.");
    deser.SetWireFormatVersion(*version);
  }
  KJ_IF_MAYBE(arrayBuffers, transferedArrayBuffers) {
    for (auto n : kj::indices(*arrayBuffers)) {
      deser.TransferArrayBuffer(n,
          v8::ArrayBuffer::New(js.v8Isolate, kj::mv((*arrayBuffers)[n])));
    }
  }
}

v8::Local<v8::Value> Deserializer::readValue(Lock& js) {
  return check(deser.ReadValue(js.v8Context()));
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
    Lock& js,
    v8::Local<v8::Value> value,
    kj::Maybe<kj::ArrayPtr<jsg::Value>> maybeTransfer) {
  Serializer ser(js, nullptr);
  KJ_IF_MAYBE(transfers, maybeTransfer) {
    for (auto& item : *transfers) {
      auto val = item.getHandle(js);
      JSG_REQUIRE(val->IsArrayBuffer(), TypeError, "Object is not transferable");
      ser.transfer(js, val.As<v8::ArrayBuffer>());
    }
  }
  ser.write(js, value);
  auto released = ser.release();
  Deserializer des(js, released, nullptr);
  return des.readValue(js);
}

}  // namespace workerd::jsg
