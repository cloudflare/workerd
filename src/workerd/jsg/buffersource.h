// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "jsg.h"

namespace workerd::jsg {

#define JSG_ARRAY_BUFFER_VIEW_TYPES(V)                                                            \
  V(Uint8Array, 1, true)                                                                           \
  V(Uint8ClampedArray, 1, true)                                                                    \
  V(Uint16Array, 2, true)                                                                          \
  V(Uint32Array, 4, true)                                                                          \
  V(Int8Array, 1, true)                                                                            \
  V(Int16Array, 2, true)                                                                           \
  V(Int32Array, 4, true)                                                                           \
  V(Float32Array, 4, false)                                                                        \
  V(Float64Array, 8, false)                                                                        \
  V(BigInt64Array, 8, true)                                                                        \
  V(BigUint64Array, 8, true)

template <typename T>
concept BufferSourceType = requires(T a) {
  kj::isSameType<v8::ArrayBuffer, T>() || std::is_base_of<v8::ArrayBufferView, T>::value;
};

template <BufferSourceType T>
static constexpr size_t getBufferSourceElementSize() {
  if constexpr (kj::isSameType<v8::Uint8Array, T>() ||
                kj::isSameType<v8::Uint8ClampedArray, T>() ||
                kj::isSameType<v8::Int8Array, T>() ||
                kj::isSameType<v8::DataView, T>() ||
                kj::isSameType<v8::ArrayBuffer, T>() ||
                kj::isSameType<v8::ArrayBufferView, T>() ||
                kj::isSameType<v8::TypedArray, T>()) {
    return 1;
  }
#define V(Type, size, _) else if constexpr (kj::isSameType<v8::Type, T>()) { return size; }
  JSG_ARRAY_BUFFER_VIEW_TYPES(V)
#undef V
  asm("no_matching_buffer_view_type\n");
}

template <BufferSourceType T>
static constexpr size_t checkIsIntegerType() {
  if constexpr (kj::isSameType<v8::ArrayBuffer, T>() ||
                kj::isSameType<v8::DataView, T>() ||
                kj::isSameType<v8::ArrayBufferView, T>()) {
    return false;
  } else if constexpr (kj::isSameType<v8::TypedArray, T>()) {
    return true;
  }
#define V(Type, _, res) else if constexpr (kj::isSameType<v8::Type, T>()) { return res; }
  JSG_ARRAY_BUFFER_VIEW_TYPES(V)
#undef V
  asm("no_matching_buffer_view_type\n");
}

class BufferSource;
class BackingStore;
using BufferSourceViewConstructor = v8::Local<v8::Value>(*)(Lock&, BackingStore&);

// The jsg::BackingStore wraps a v8::BackingStore and retains information about the
// type of ArrayBuffer or ArrayBufferView to which it is associated. Namely, it records
// the byte length, offset, element size, and constructor type allowing the view to be
// recreated.
//
// The BackingStore can be safely used outside of the isolate lock and can even be passed
// into another isolate if necessary.
class BackingStore {
public:
  template <BufferSourceType T = v8::Uint8Array>
  static BackingStore from(kj::Array<kj::byte> data) {
    // Creates a new BackingStore that takes over ownership of the given kj::Array.
    size_t size = data.size();
    auto ptr = new kj::Array<byte>(kj::mv(data));
    return BackingStore(
        v8::ArrayBuffer::NewBackingStore(
            ptr->begin(), size,
            [](void*, size_t, void* ptr) {
              delete reinterpret_cast<kj::Array<byte>*>(ptr);
            }, ptr),
        size, 0,
        getBufferSourceElementSize<T>(), construct<T>,
        checkIsIntegerType<T>());
  }

  // Creates a new BackingStore of the given size.
  template <BufferSourceType T = v8::Uint8Array>
  static BackingStore alloc(Lock& js, size_t size) {
    return BackingStore(
        v8::ArrayBuffer::NewBackingStore(js.v8Isolate, size),
        size, 0,
        getBufferSourceElementSize<T>(), construct<T>,
        checkIsIntegerType<T>());
  }

  using Disposer = void(void*,size_t,void*);

  // Creates and returns a BackingStore that wraps an external data pointer
  // with a custom disposer.
  template <BufferSourceType T = v8::Uint8Array>
  static BackingStore wrap(void* data, size_t size, Disposer disposer, void* ctx) {
    return BackingStore(
        v8::ArrayBuffer::NewBackingStore(data, size, disposer, ctx),
        size, 0,
        getBufferSourceElementSize<T>(), construct<T>,
        checkIsIntegerType<T>());
  }

  explicit BackingStore(
      std::shared_ptr<v8::BackingStore> backingStore,
      size_t byteLength,
      size_t byteOffset,
      size_t elementSize,
      BufferSourceViewConstructor ctor,
      bool integerType);

  BackingStore(BackingStore&& other) = default;
  BackingStore& operator=(BackingStore&& other) = default;
  KJ_DISALLOW_COPY(BackingStore);

  inline kj::ArrayPtr<kj::byte> asArrayPtr() KJ_LIFETIMEBOUND {
    KJ_ASSERT(backingStore != nullptr, "Invalid access after move.");
    return kj::ArrayPtr<kj::byte>(
        static_cast<kj::byte*>(backingStore->Data()) + byteOffset,
        byteLength);
  }

  inline operator kj::ArrayPtr<kj::byte>() KJ_LIFETIMEBOUND { return asArrayPtr(); }

  bool operator==(const BackingStore& other);

  inline const kj::ArrayPtr<const kj::byte> asArrayPtr() const KJ_LIFETIMEBOUND {
    KJ_ASSERT(backingStore != nullptr, "Invalid access after move.");
    return kj::ArrayPtr<kj::byte>(
        static_cast<kj::byte*>(backingStore->Data()) + byteOffset,
        byteLength);
  }

  inline operator const kj::ArrayPtr<const kj::byte>() const KJ_LIFETIMEBOUND {
    return asArrayPtr();
  }

  inline size_t size() const { return byteLength; };
  inline size_t getOffset() const { return byteOffset; }
  inline size_t getElementSize() const { return elementSize; }
  inline bool isIntegerType() const { return integerType; }

  // Creates a new BackingStore as a view over the same underlying v8::BackingStore
  // but with different handle type information. This is required, for instance, in
  // use cases like the Streams API where we have to be able to surface a Uint8Array
  // view over the BackingStore to fulfill a BYOB read while maintaining the original
  // type information to recreate the original type of view once the read is complete.
  template <BufferSourceType T = v8::Uint8Array>
  BackingStore getTypedView() {
    return BackingStore(
        backingStore,
        byteLength,
        byteOffset,
        getBufferSourceElementSize<T>(),
        construct<T>,
        checkIsIntegerType<T>());
  }

  template <BufferSourceType T = v8::Uint8Array>
  BackingStore getTypedViewSlice(size_t start, size_t end) {
    KJ_ASSERT(start <= end);
    auto length = end - start;
    auto startOffset = byteOffset + start;
    KJ_ASSERT(length <= byteLength);
    KJ_ASSERT(startOffset <= backingStore->ByteLength());
    KJ_ASSERT(startOffset + length <= backingStore->ByteLength());
    return BackingStore(
        backingStore,
        length,
        startOffset,
        getBufferSourceElementSize<T>(),
        construct<T>,
        checkIsIntegerType<T>());
  }

  inline v8::Local<v8::Value> createHandle(Lock& js) {
    return ctor(js, *this);
  }

  // Shrinks the effective size of the backing store by a number of bytes off
  // the front of the data. Useful when incrementally consuming the data as
  // we do in the streams implementation.
  inline void consume(size_t bytes) {
    KJ_ASSERT(bytes <= byteLength);
    byteOffset += bytes;
    byteLength -= bytes;
  }

  // Shrinks the effective size of the backing store by a number of bytes off
  // the end of the data. Useful when a more limited view of the buffer is
  // required (such as when fulfilling partial stream reads).
  inline void trim(size_t bytes) {
    KJ_ASSERT(bytes <= byteLength);
    byteLength -= bytes;
  }

  inline BackingStore clone() {
    return BackingStore(backingStore, byteLength, byteOffset, elementSize, ctor, integerType);
  }

private:
  std::shared_ptr<v8::BackingStore> backingStore;
  size_t byteLength;
  size_t byteOffset;
  size_t elementSize;

  // The ctor here is a pointer to a static template function that can create a
  // new type-specific instance of the JavaScript ArrayBuffer or ArrayBufferView wrapper
  // for the backing store. The specific type of constructor to store is determined
  // when the BufferSource instance is created and it is used only if getHandle() is
  // called on a BufferSource that has been detached.
  BufferSourceViewConstructor ctor;

  bool integerType;

  template <BufferSourceType T>
  static v8::Local<v8::Value> construct(Lock& js, BackingStore& store) {
    if constexpr (kj::isSameType<v8::ArrayBuffer, T>()) {
      return v8::ArrayBuffer::New(js.v8Isolate, store.backingStore);
    } else if constexpr (kj::isSameType<v8::ArrayBufferView, T>()) {
      return v8::DataView::New(
          v8::ArrayBuffer::New(js.v8Isolate, store.backingStore),
          store.byteOffset, store.byteLength);
    } else if constexpr (kj::isSameType<v8::TypedArray, T>()) {
      return v8::Uint8Array::New(
          v8::ArrayBuffer::New(js.v8Isolate, store.backingStore),
          store.byteOffset, store.byteLength);
    } else {
      return T::New(
          v8::ArrayBuffer::New(js.v8Isolate, store.backingStore),
          store.byteOffset,
          store.byteLength / store.elementSize);
    }
  }

  friend class BufferSource;
};

// A BufferSource is an abstraction for v8::ArrayBuffer and v8::ArrayBufferView types.
// It has a couple of significant features relative to the alternative mapping between
// kj::Array<kj::byte> and ArrayBuffer/ArrayBufferView:
//
//  * A BufferSource created from an ArrayBuffer/ArrayBufferView maintains a reference
//    to JavaScript object, ensuring that when the BufferSource is passed back
//    out to JavaScript, the same object will be returned.
//  * A BufferSource can detach the BackingStore from the ArrayBuffer/ArrayBufferView.
//    When doing so, the BackingStore is removed from the BufferSource and the association
//    with the ArrayBuffer/ArrayBufferView is severed.
//
// When an object holds a reference to a BufferSource (e.g. as a member variable), it
// must implement visitForGc and ensure the BufferSource is properly visited,
//
// As a side note, the name "BufferSource" comes from the Web IDL spec.
//
// How to use it:
//
// In methods that are exposed to JavaScript, specify jsg::BufferSource as the type:
// e.g.
//
//   class MyAPiObject: public jsg::Object {
//   public:
//     jsg::BufferSource foo(jsg::Lock& js, jsg::BufferSource source) {
//       // While the BufferSource is attached, you can access the data as an
//       // kj::ArrayPtr...
//       {
//         auto ptr = kj::ArrayPtr<kj::byte>(source);
//       }
//
//       // Or, you can detach the jsg::BackingStore from the BufferSource.
//       auto backingStore = source.detach();
//       auto ptr = kj::ArrayPtr<kj::byte>(backingStore);
//       // Do something with ptr...
//       return BufferSource(js, kj::mv(backingStore));
//     }
//   };
class BufferSource {
public:
  static kj::Maybe<BufferSource> tryAlloc(Lock& js, size_t size);
  static BufferSource wrap(Lock& js, void* data, size_t size,
                           BackingStore::Disposer disposer, void* ctx);

  // Create a new BufferSource that takes over ownership of the given BackingStore.
  explicit BufferSource(Lock& js, BackingStore&& backingStore);

  // Create a BufferSource from the given JavaScript handle.
  explicit BufferSource(Lock& js, v8::Local<v8::Value> handle);

  BufferSource(BufferSource&&) = default;
  BufferSource& operator=(BufferSource&&) = default;

  KJ_DISALLOW_COPY(BufferSource);

  // True if the BackingStore has been removed from this BufferSource.
  inline bool isDetached() const { return maybeBackingStore == nullptr; }

  bool canDetach(Lock& js);

  // Removes the BackingStore from the BufferSource and severs its connection to
  // the ArrayBuffer/ArrayBufferView handle.
  // It's worth mentioning that detach can throw application-visible exceptions
  // in the case the ArrayBuffer cannot be detached. Any detaching should be
  // performed as early as possible in an API method implementation.
  BackingStore detach(Lock& js, kj::Maybe<v8::Local<v8::Value>> maybeKey = nullptr);

  v8::Local<v8::Value> getHandle(Lock& js);

  inline kj::ArrayPtr<kj::byte> asArrayPtr() KJ_LIFETIMEBOUND {
    return KJ_ASSERT_NONNULL(maybeBackingStore).asArrayPtr();
  }

  inline operator kj::ArrayPtr<kj::byte>() KJ_LIFETIMEBOUND { return asArrayPtr(); }

  inline const kj::ArrayPtr<const kj::byte> asArrayPtr() const KJ_LIFETIMEBOUND {
    return KJ_ASSERT_NONNULL(maybeBackingStore).asArrayPtr();
  }

  inline operator const kj::ArrayPtr<const kj::byte>() const KJ_LIFETIMEBOUND {
    return asArrayPtr();
  }

  inline size_t size() const {
    return KJ_ASSERT_NONNULL(maybeBackingStore).size();
  };

  inline kj::Maybe<size_t> underlyingArrayBufferSize(Lock& js) {
    if (isDetached()) {
      return nullptr;
    }
    auto h = getHandle(js);
    if (h->IsArrayBuffer()) {
      return h.As<v8::ArrayBuffer>()->ByteLength();
    } else if (h->IsArrayBufferView()) {
      return h.As<v8::ArrayBufferView>()->Buffer()->ByteLength();
    }
    KJ_UNREACHABLE;
  }

  inline size_t getOffset() const {
    return KJ_ASSERT_NONNULL(maybeBackingStore).getOffset();
  }

  inline size_t getElementSize() const {
    return KJ_ASSERT_NONNULL(maybeBackingStore).getElementSize();
  }

  // Some standard APIs that use BufferSource / ArrayBufferView are limited to just
  // supported "Integer-type ArrayBufferViews". As a convenience, when the BufferSource
  // is created, we record whether or not the type qualifies as an integer type.
  inline bool isIntegerType() const {
    return KJ_ASSERT_NONNULL(maybeBackingStore).isIntegerType();
  }

  // Sets the detach key that must be provided with the detach(...) method
  // to successfully detach the backing store.
  void setDetachKey(Lock& js, v8::Local<v8::Value> key);

private:
  Value handle;
  kj::Maybe<BackingStore> maybeBackingStore;

  static auto determineConstructor(auto& value) {
    if (value->IsArrayBuffer()) {
      return BackingStore::construct<v8::ArrayBuffer>;
    } else if (value->IsDataView()) {
      return BackingStore::construct<v8::DataView>;
    }
#define V(Type, _, __) else if (value->Is##Type()) return BackingStore::construct<v8::Type>;
    JSG_ARRAY_BUFFER_VIEW_TYPES(V)
#undef V
    KJ_UNREACHABLE;
  }

  friend class BackingStore;
  friend class GcVisitor;
};

// TypeWrapper implementation for the BufferSource type.
template <typename TypeWrapper>
class BufferSourceWrapper {
public:
  static constexpr const char* getName(BufferSource*) { return "BufferSource"; }

  v8::Local<v8::Value> wrap(
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      BufferSource bufferSource) {
    return bufferSource.getHandle(Lock::from(context->GetIsolate()));
  }

  kj::Maybe<BufferSource> tryUnwrap(
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      BufferSource*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    if (!handle->IsArrayBuffer() && !handle->IsArrayBufferView()) {
      return nullptr;
    }
    return BufferSource(Lock::from(context->GetIsolate()), handle);
  }
};

inline BufferSource Lock::arrayBuffer(kj::Array<kj::byte> data) {
  return BufferSource(*this, BackingStore::from<v8::ArrayBuffer>(kj::mv(data)));
}

}  // namespace workerd::jsg
