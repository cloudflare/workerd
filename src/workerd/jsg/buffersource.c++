// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "buffersource.h"

namespace workerd::jsg {

namespace {
auto getBacking(auto& handle) {
  auto buffer = handle->IsArrayBuffer() ?
      handle.template As<v8::ArrayBuffer>() :
      handle.template As<v8::ArrayBufferView>()->Buffer();
  return buffer->GetBackingStore();
}

size_t getByteLength(auto& handle) {
  return handle->IsArrayBuffer() ?
      handle.template As<v8::ArrayBuffer>()->ByteLength() :
      handle.template As<v8::ArrayBufferView>()->ByteLength();
}

size_t getByteOffset(auto& handle) {
  return handle->IsArrayBuffer() ? 0 : handle.template As<v8::ArrayBufferView>()->ByteOffset();
}

auto determineElementSize(auto& handle) {
#define V(Type, size, _) if (handle->Is##Type()) return size;
  JSG_ARRAY_BUFFER_VIEW_TYPES(V)
#undef V
  KJ_ASSERT(handle->IsDataView() || handle->IsArrayBuffer());
  return 1;
}

bool isDetachable(auto handle) {
  auto buffer = handle->IsArrayBuffer() ?
      handle.template As<v8::ArrayBuffer>() :
      handle.template As<v8::ArrayBufferView>()->Buffer();
  return buffer->IsDetachable();
}

bool determineIsIntegerType(auto& handle) {
#define V(Type, _, integerView) if (handle->Is##Type()) return integerView;
  JSG_ARRAY_BUFFER_VIEW_TYPES(V);
#undef V
  return false;
}

Value createHandle(Lock& js, BackingStore& backingStore) {
  return js.withinHandleScope([&] {
    return js.v8Ref(backingStore.createHandle(js));
  });
}

}  // namespace

void GcVisitor::visit(BufferSource& value)  {
  visit(value.handle);
}

BackingStore::BackingStore(
    std::shared_ptr<v8::BackingStore> backingStore,
    size_t byteLength,
    size_t byteOffset,
    size_t elementSize,
    BufferSourceViewConstructor ctor,
    bool integerType)
    : backingStore(kj::mv(backingStore)),
      byteLength(byteLength),
      byteOffset(byteOffset),
      elementSize(elementSize),
      ctor(ctor),
      integerType(integerType) {
  KJ_REQUIRE(this->backingStore != nullptr);
  KJ_REQUIRE(this->byteLength <= this->backingStore->ByteLength());
  KJ_REQUIRE(this->byteLength % this->elementSize == 0,
              kj::str("byteLength must be a multiple of ", this->elementSize, "."));
}

bool BackingStore::operator==(const BackingStore& other) {
  return backingStore == other.backingStore &&
         byteLength == other.byteLength &&
         byteOffset == other.byteOffset;
}

kj::Maybe<BufferSource> BufferSource::tryAlloc(Lock& js, size_t size) {
  v8::Local<v8::ArrayBuffer> buffer;
  if (v8::ArrayBuffer::MaybeNew(js.v8Isolate, size).ToLocal(&buffer)) {
    return BufferSource(js, v8::Uint8Array::New(buffer, 0, size).As<v8::Value>());
  }
  return nullptr;
}

BufferSource::BufferSource(Lock& js, v8::Local<v8::Value> handle)
    : handle(js.v8Ref(handle)),
      maybeBackingStore(BackingStore(
          getBacking(handle),
          getByteLength(handle),
          getByteOffset(handle),
          determineElementSize(handle),
          determineConstructor(handle),
          determineIsIntegerType(handle))) {}

BufferSource::BufferSource(
    Lock& js,
    BackingStore&& backingStore)
    : handle(createHandle(js, backingStore)),
      maybeBackingStore(kj::mv(backingStore)) {}

BackingStore BufferSource::detach(Lock& js, kj::Maybe<v8::Local<v8::Value>> maybeKey) {
  auto theHandle = handle.getHandle(js);
  JSG_REQUIRE(isDetachable(theHandle),
               TypeError,
               "This BufferSource does not have a detachable backing store.");
  auto backingStore =
      kj::mv(JSG_REQUIRE_NONNULL(maybeBackingStore,
                                  TypeError,
                                  "This BufferSource has already been detached."));
  maybeBackingStore = nullptr;

  v8::Local<v8::Value> key = maybeKey.orDefault(v8::Local<v8::Value>());

  auto buffer = theHandle->IsArrayBuffer() ?
      theHandle.As<v8::ArrayBuffer>() :
      theHandle.As<v8::ArrayBufferView>()->Buffer();
  jsg::check(buffer->Detach(key));

  return kj::mv(backingStore);
}

bool BufferSource::canDetach(Lock& js) {
  if (isDetached()) return false;
  return isDetachable(handle.getHandle(js));
}

v8::Local<v8::Value> BufferSource::getHandle(Lock& js) {
  return handle.getHandle(js);
}

void BufferSource::setDetachKey(Lock& js, v8::Local<v8::Value> key) {
  auto handle = getHandle(js);
  auto buffer = handle->IsArrayBuffer() ?
      handle.As<v8::ArrayBuffer>() :
      handle.As<v8::ArrayBufferView>()->Buffer();
  buffer->SetDetachKey(key);
}

BufferSource BufferSource::wrap(Lock& js, void* data, size_t size,
                                BackingStore::Disposer disposer, void* ctx) {
  return BufferSource(js, BackingStore::wrap(data, size, disposer, ctx));
}

}  // namespace workerd::jsg
