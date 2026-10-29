// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Interop between Rust JSG resources and C++ JSG resources.
//
// - Rust holds a C++ JSG resource as a `jsg_cpp::Ref<T>`, which owns a `CppRef`: a heap-allocated
//   `jsg::Ref` that Rust's GC tracing visits as C++ tracing would. The per-type shims that convert
//   one to and from JavaScript instantiate `cppRefTryUnwrap<T>()` and `cppRefWrap<T>()`, which go
//   through the isolate's `TypeHandler<jsg::Ref<T>>`, so subclasses, wrapper identity and
//   prototype checks are exactly C++ JSG's.
// - Rust calls a method of a C++ resource through a per-method shim that runs the call in
//   `callCpp()`, the same boundary a JSG method callback has.
// - C++ holds a Rust JSG resource as a `RustRef<Tag>`, and `RustResourceWrapper` lets C++ JSG
//   signatures (methods, `jsg::Function`, `jsg::Constructor`, `kj::OneOf`, `jsg::Optional`)
//   take and return one.
//
// This file and ffi.c++ are the only Rust JSG shims allowed to use C++ JSG, and only the
// constructs in INTEROP_CONSTRUCTS of src/rust/jsg/cpp-jsg-allowlist-test.py. See
// src/rust/jsg/README.md, "Safety rules", rule 3.

#include <workerd/jsg/jsg.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/jsg/ffi.h>

#include <kj-rs/kj-rs.h>
#include <rust/cxx.h>

#include <kj/memory.h>

namespace workerd::rust::jsg_cpp {

using Isolate = v8::Isolate;
using Local = ::workerd::rust::jsg::Local;
using MaybeLocal = ::workerd::rust::jsg::MaybeLocal;
using GcVisitor = ::workerd::rust::jsg::GcVisitor;

// A reference from Rust to a C++ JSG resource: what a `jsg_cpp::Ref<T>` owns.
//
// It is a `jsg::Ref` at a stable address, so that GC tracing can record its parent and strength
// as for a `jsg::Ref` field of a C++ resource. `object` is the `T*` the reference was made for,
// which Rust dereferences as its opaque `T`; it is kept beside the type-erased `jsg::Ref<Object>`
// so that neither Rust nor this class needs to know `T`.
class CppRef final {
 public:
  template <typename T>
  explicit CppRef(::workerd::jsg::Ref<T> typed): object(typed.get()),
                                                 ref(kj::mv(typed)) {}

  template <typename T>
  T& get() const {
    return *static_cast<T*>(object);
  }

  // The `T*`, for Rust to dereference as its opaque `T`.
  void* untypedObject() const {
    return object;
  }

  // A new strong reference to the same object.
  kj::Own<CppRef> clone() const {
    return kj::heap<CppRef>(object, ref.addRef());
  }

  void visitForGc(::workerd::jsg::GcVisitor& visitor) const {
    visitor.visit(ref);
  }

  // Public for kj::heap.
  CppRef(void* object, ::workerd::jsg::Ref<::workerd::jsg::Object> ref)
      : object(object),
        ref(kj::mv(ref)) {}

 private:
  void* object;
  // Mutable because GC tracing updates its parent and strength through `const CppRef&`, as Rust's
  // `Traced::trace` takes `&self`.
  mutable ::workerd::jsg::Ref<::workerd::jsg::Object> ref;
};

// FFI for `jsg_cpp::Ref<T>` (ffi.rs).
kj::Own<CppRef> cpp_ref_clone(const CppRef& ref);
size_t cpp_ref_object(const CppRef& ref);
void cpp_ref_visit(GcVisitor* visitor, const CppRef& ref);

// Runs `func(jsg::Lock&)` the way a JSG method callback runs C++ API code (jsg::liftKj()), and
// returns whether it completed. A `jsg::JsExceptionThrown` leaves its exception pending. A
// `kj::Exception` becomes the JavaScript exception C++ JSG would throw for it: the error of a
// `JSG_REQUIRE` or a tunneled exception, a DOMException, or an internal error. Unlike liftKj(),
// it throws nothing while execution is terminating, which would replace the termination.
//
// Every per-method shim that Rust calls runs its C++ in here, so that no C++ exception crosses
// the bridge and Rust sees every failure as an exception pending in V8 (`jsg::Error::pending()`).
//
// The isolate must be locked by this thread, with a context entered and a HandleScope open.
template <typename Func>
bool callCpp(Isolate* isolate, Func&& func) noexcept {
  auto throwUnlessTerminating = [&](auto&& throwIt) {
    if (!isolate->IsExecutionTerminating()) {
      throwIt();
    }
  };
  try {
    try {
      func(::workerd::jsg::Lock::from(isolate));
      return true;
    } catch (kj::Exception& exception) {
      // Building the JavaScript error can throw JsExceptionThrown itself, which the outer catch
      // swallows, as in liftKj().
      throwUnlessTerminating(
          [&] { ::workerd::jsg::throwInternalError(isolate, kj::mv(exception)); });
    }
  } catch (::workerd::jsg::JsExceptionThrown&) {
    // The exception is pending.
  } catch (std::exception& exception) {
    throwUnlessTerminating([&] { ::workerd::jsg::throwInternalError(isolate, exception.what()); });
  } catch (...) {
    throwUnlessTerminating([&] {
      ::workerd::jsg::throwInternalError(
          isolate, kj::str("caught unknown exception of type: ", kj::getCaughtExceptionType()));
    });
  }
  return false;
}

// callCpp() for a shim that returns a C++ resource to Rust: `func(jsg::Lock&)` returns a
// `jsg::Ref<T>` or a `kj::Maybe<jsg::Ref<T>>` (`jsg::Optional`), which is stored in `out` for
// `jsg_cpp::call_cpp_for_ref::<T>()`.
template <typename T, typename Func>
bool callCppForRef(Isolate* isolate, kj::Maybe<kj::Own<CppRef>>& out, Func&& func) noexcept {
  return callCpp(isolate, [&](::workerd::jsg::Lock& js) {
    kj::Maybe<::workerd::jsg::Ref<T>> result = func(js);
    out = kj::mv(result).map(
        [](::workerd::jsg::Ref<T>&& typed) { return kj::heap<CppRef>(kj::mv(typed)); });
  });
}

namespace _ {
template <typename T>
const ::workerd::jsg::TypeHandler<::workerd::jsg::Ref<T>>& typeHandler(::workerd::jsg::Lock& js) {
  return KJ_REQUIRE_NONNULL(js.tryGetTypeHandler<::workerd::jsg::Ref<T>>(),
      "C++ JSG resource type is not registered with this isolate", typeid(T).name());
}
}  // namespace _

// The `try_unwrap` of a `jsg_cpp::CppResource`: a reference to the `T` that `value` wraps, if it
// wraps a `T` or a subclass of `T`, as C++ JSG unwraps a `jsg::Ref<T>` parameter. Runs no
// JavaScript. Throws only if `T` is not registered with the isolate, which is a bug.
template <typename T>
kj::Maybe<kj::Own<CppRef>> cppRefTryUnwrap(Isolate* isolate, Local value) {
  auto& js = ::workerd::jsg::Lock::from(isolate);
  return _::typeHandler<T>(js)
      .tryUnwrap(js, ::workerd::rust::jsg::local_from_ffi<v8::Value>(kj::mv(value)))
      .map([](::workerd::jsg::Ref<T>&& typed) { return kj::heap<CppRef>(kj::mv(typed)); });
}

// The `wrap` of a `jsg_cpp::CppResource`: the JavaScript wrapper of `ref`, which must have been
// made for a `T`. As for a `jsg::Ref<T>` C++ JSG returns, an existing wrapper is reused, and a new
// one is made for the object's dynamic type, a subclass of `T` included. Empty, with an exception
// pending, if that fails.
template <typename T>
MaybeLocal cppRefWrap(Isolate* isolate, const CppRef& ref) {
  v8::MaybeLocal<v8::Value> result;
  callCpp(isolate, [&](::workerd::jsg::Lock& js) {
    result = _::typeHandler<T>(js).wrap(js, ::workerd::jsg::Ref<T>(kj::addRef(ref.get<T>())));
  });
  return ::workerd::rust::jsg::maybe_local_to_ffi(result);
}

// A reference from C++ to a Rust JSG resource, typed by `Tag`. `Tag` is a C++ struct that
// stands for the Rust resource type:
//
//     struct MyResourceTag {
//       // The JavaScript class name, for error messages.
//       static constexpr const char* NAME = "MyResource";
//       // An `extern "Rust"` function returning `jsg::resource::ffi_is::<MyResource>()`.
//       static kj::Maybe<bool> isInstance(v8::Isolate* isolate, Local value);
//     };
//
// It holds the resource's JavaScript wrapper, which owns the resource, so it keeps both alive,
// preserves the wrapper's identity, and is traced like any `jsg::V8Ref`: a cycle through it, for
// example back from the Rust resource through a `jsg_cpp::Ref`, is collected. C++ calls the
// resource's methods through `extern "Rust"` functions given `toFfi()`.
template <typename Tag>
class RustRef {
 public:
  // `value` if it is the wrapper of a `Tag` resource, kj::none for any other value. Throws
  // JsExceptionThrown if the check left an exception pending.
  static kj::Maybe<RustRef> tryFrom(::workerd::jsg::Lock& js, v8::Local<v8::Value> value) {
    KJ_IF_SOME(isInstance,
        Tag::isInstance(js.v8Isolate, ::workerd::rust::jsg::to_ffi(kj::cp(value)))) {
      if (!isInstance) return kj::none;
      return RustRef(js.v8Ref(value.As<v8::Object>()));
    }
    throw ::workerd::jsg::JsExceptionThrown();
  }

  // The result of a Rust constructor for C++ (an `extern "Rust"` function returning
  // `jsg::v8::run_ffi_callback`'s result). Throws JsExceptionThrown if it is empty, meaning the
  // constructor threw.
  static RustRef fromFfi(::workerd::jsg::Lock& js, MaybeLocal created) {
    v8::Local<v8::Value> value;
    if (!::workerd::rust::jsg::maybe_local_from_ffi<v8::Value>(kj::mv(created)).ToLocal(&value)) {
      throw ::workerd::jsg::JsExceptionThrown();
    }
    return KJ_REQUIRE_NONNULL(tryFrom(js, value), "a Rust constructor returned another type");
  }

  v8::Local<v8::Object> getHandle(::workerd::jsg::Lock& js) const {
    return wrapper.getHandle(js);
  }

  // The wrapper, to pass to an `extern "Rust"` function that calls a method of the resource.
  Local toFfi(::workerd::jsg::Lock& js) const {
    return ::workerd::rust::jsg::to_ffi(v8::Local<v8::Value>(getHandle(js)));
  }

  RustRef addRef(::workerd::jsg::Lock& js) {
    return RustRef(wrapper.addRef(js));
  }

  void visitForGc(::workerd::jsg::GcVisitor& visitor) {
    visitor.visit(wrapper);
  }

 private:
  explicit RustRef(::workerd::jsg::V8Ref<v8::Object> wrapper): wrapper(kj::mv(wrapper)) {}

  ::workerd::jsg::V8Ref<v8::Object> wrapper;
};

// A TypeWrapper extension that converts `RustRef<Tag>`s, for every `Tag`, so that C++ JSG
// signatures can take and return Rust resources. Add it to the isolate's types:
//
//     JSG_DECLARE_ISOLATE_TYPE(MyIsolate, ...,
//         jsg::TypeWrapperExtension<rust::jsg_cpp::RustResourceWrapper>);
template <typename TypeWrapper>
class RustResourceWrapper {
 public:
  template <typename Tag>
  static constexpr const char* getName(RustRef<Tag>*) {
    return Tag::NAME;
  }

  template <typename Tag>
  v8::Local<v8::Value> wrap(::workerd::jsg::Lock& js,
      v8::Local<v8::Context>,
      kj::Maybe<v8::Local<v8::Object>>,
      RustRef<Tag> value) {
    return value.getHandle(js);
  }

  template <typename Tag>
  kj::Maybe<RustRef<Tag>> tryUnwrap(::workerd::jsg::Lock& js,
      v8::Local<v8::Context>,
      v8::Local<v8::Value> handle,
      RustRef<Tag>*,
      kj::Maybe<v8::Local<v8::Object>>) {
    return RustRef<Tag>::tryFrom(js, handle);
  }

  void newContext() = delete;
  void getTemplate() = delete;
};

}  // namespace workerd::rust::jsg_cpp
