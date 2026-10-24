// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// This header contains template implementations that require complete type definitions.
// It should be included after workerd/rust/jsg/v8.rs.h which defines the complete types.

#include <workerd/jsg/util.h>
#include <workerd/rust/jsg/ffi.h>
#include <workerd/rust/jsg/v8.rs.h>

namespace workerd::rust::jsg {

// Local<T>
// Rust's `jsg::v8::Local` is a non-null pointer with this layout, and `Option<Local>` a
// nullable one, which matches `v8::MaybeLocal<T>` below.
static_assert(sizeof(v8::Local<v8::Value>) == 8, "Size should match");
static_assert(alignof(v8::Local<v8::Value>) == 8, "Alignment should match");
static_assert(sizeof(v8::Local<v8::Value>) == sizeof(Local), "Size should match");
static_assert(alignof(v8::Local<v8::Value>) == alignof(Local), "Alignment should match");
// Rust reads an empty handle as null (`None`). With direct handles V8 uses a non-null empty
// value instead, which Rust would take for a real handle.
static_assert(
    v8::internal::ValueHelper::kEmpty == v8::internal::ValueHelper::InternalRepresentationType{},
    "an empty v8::Local must be a null pointer");

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

// MaybeLocal<T>
// v8::MaybeLocal<T> is exactly one Local<T> field at offset 0, so it has the same
// size and layout as ffi::Local (one pointer-sized word, zero means empty).
static_assert(sizeof(v8::MaybeLocal<v8::Value>) == sizeof(MaybeLocal), "Size should match");
static_assert(alignof(v8::MaybeLocal<v8::Value>) == alignof(MaybeLocal), "Alignment should match");

template <typename T>
inline MaybeLocal maybe_local_to_ffi(v8::MaybeLocal<T> value) {
  size_t result;
  auto ptr_void = reinterpret_cast<void*>(&result);
  new (ptr_void) v8::MaybeLocal<T>(value);
  return MaybeLocal{result};
}

template <typename T>
inline v8::MaybeLocal<T> maybe_local_from_ffi(MaybeLocal&& value) {
  auto ptr_void = reinterpret_cast<void*>(&value.ptr);
  return *reinterpret_cast<v8::MaybeLocal<T>*>(ptr_void);
}

// Global<T>
//
// ffi::Global stores only the strong v8::Global<v8::Value> in `ptr`.
// The traced v8::TracedReference<v8::Data> slot lives next to it in the
// GlobalSlot of a TracedGlobal<T> on the Rust side, passed by reference to
// wrappable_visit_global.
static_assert(sizeof(v8::Global<v8::Value>) == sizeof(size_t), "Global must be pointer-sized");
static_assert(sizeof(v8::TracedReference<v8::Data>) == sizeof(size_t),
    "TracedReference must be pointer-sized");
static_assert(sizeof(Global) == sizeof(size_t), "ffi::Global must hold exactly one pointer slot");
static_assert(alignof(v8::Global<v8::Value>) == alignof(Global), "Alignment should match");

template <typename T>
inline Global to_ffi(v8::Global<T>&& value) {
  size_t strong_slot;
  auto ptr_void = reinterpret_cast<void*>(&strong_slot);
  new (ptr_void) v8::Global<T>(kj::mv(value));
  return Global{strong_slot};
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

// TracedReference
static_assert(sizeof(v8::TracedReference<v8::Data>) == sizeof(TracedReference),
    "TracedReference size must match");
static_assert(alignof(v8::TracedReference<v8::Data>) == alignof(TracedReference),
    "TracedReference alignment must match");

inline v8::TracedReference<v8::Data>& traced_ref_from_ffi(TracedReference& value) {
  return *reinterpret_cast<v8::TracedReference<v8::Data>*>(&value);
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

// Exceptions
//
// A shim that can run JavaScript takes a `MaybeLocal& exception`, and Rust calls it through
// `jsg::v8::catching()`, which turns its failure into a `jsg::Error` holding what JavaScript threw.

// Call after JavaScript run under `tryCatch` threw. Stores what it threw in `exception`, unless
// execution is terminating: then rethrows the termination, so that it keeps propagating, and leaves
// `exception` empty. This is the distinction JSG_CATCH makes: a termination cannot be caught.
inline void catchException(v8::TryCatch& tryCatch, MaybeLocal& exception) {
  if (tryCatch.CanContinue() && tryCatch.HasCaught() && !tryCatch.Exception().IsEmpty()) {
    exception = maybe_local_to_ffi(v8::MaybeLocal<v8::Value>(tryCatch.Exception()));
  } else {
    tryCatch.ReThrow();
  }
}

// Runs `fn` and returns its result. If it throws JsExceptionThrown, which C++ JSG code throws when
// JavaScript has thrown, stores what JavaScript threw with catchException() and rethrows, so that
// workerd-cxx's Result hands Rust an error for `jsg::v8::catching()` to convert.
template <typename Func>
auto catching(v8::Isolate* isolate, MaybeLocal& exception, Func&& fn) -> decltype(fn()) {
  v8::TryCatch tryCatch(isolate);
  try {
    return fn();
  } catch (::workerd::jsg::JsExceptionThrown&) {
    catchException(tryCatch, exception);
    throw;
  }
}

}  // namespace workerd::rust::jsg
