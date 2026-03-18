// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// This header contains template implementations that require complete type definitions.
// It should be included after workerd/rust/jsg/v8.rs.h which defines the complete types.

#include <workerd/rust/jsg/ffi.h>
#include <workerd/rust/jsg/v8.rs.h>

namespace workerd::rust::jsg {

// Local<T>
static_assert(sizeof(v8::Local<v8::Value>) == 8, "Size should match");
static_assert(alignof(v8::Local<v8::Value>) == 8, "Alignment should match");

template <typename T>
inline Local to_ffi(v8::Local<T>&& value) {
  size_t result;
  auto ptr_void = reinterpret_cast<void*>(&result);
  new (ptr_void) v8::Local<T>(kj::mv(value));
  return Local{result};
}

template <typename T>
inline v8::Local<T> local_from_ffi(Local&& value) {
  auto ptr_void = reinterpret_cast<void*>(&value.ptr);
  return *reinterpret_cast<v8::Local<T>*>(ptr_void);
}

template <typename T>
inline const v8::Local<T>& local_as_ref_from_ffi(const Local& value) {
  auto ptr_void = reinterpret_cast<const void*>(&value.ptr);
  return *reinterpret_cast<const v8::Local<T>*>(ptr_void);
}

// Global<T>
static_assert(sizeof(v8::Global<v8::Value>) == sizeof(Global), "Size should match");
static_assert(alignof(v8::Global<v8::Value>) == alignof(Global), "Alignment should match");

template <typename T>
inline Global to_ffi(v8::Global<T>&& value) {
  size_t result;
  auto ptr_void = reinterpret_cast<void*>(&result);
  new (ptr_void) v8::Global<T>(kj::mv(value));
  return Global{result};
}

template <typename T>
inline v8::Global<T> global_from_ffi(Global&& value) {
  auto ptr_void = reinterpret_cast<void*>(&value.ptr);
  return kj::mv(*reinterpret_cast<v8::Global<T>*>(ptr_void));
}

template <typename T>
inline const v8::Global<T>& global_as_ref_from_ffi(const Global& value) {
  auto ptr_void = reinterpret_cast<const void*>(&value.ptr);
  return *reinterpret_cast<const v8::Global<T>*>(ptr_void);
}

template <typename T>
inline v8::Global<T>* global_as_ref_from_ffi(Global& value) {
  auto ptr_void = reinterpret_cast<void*>(&value.ptr);
  return reinterpret_cast<v8::Global<T>*>(ptr_void);
}

// GcVisitor - wraps a pointer to jsg::GcVisitor
static_assert(sizeof(::workerd::jsg::GcVisitor*) == sizeof(GcVisitor), "Size should match");
static_assert(alignof(::workerd::jsg::GcVisitor*) == alignof(GcVisitor), "Alignment should match");

inline GcVisitor to_ffi(::workerd::jsg::GcVisitor* visitor) {
  return GcVisitor{reinterpret_cast<size_t>(visitor)};
}

inline ::workerd::jsg::GcVisitor* gc_visitor_from_ffi(GcVisitor* value) {
  return reinterpret_cast<::workerd::jsg::GcVisitor*>(value->ptr);
}

}  // namespace workerd::rust::jsg
