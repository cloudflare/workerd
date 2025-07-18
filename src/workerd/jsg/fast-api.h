#pragma once

// Fast API implementation for workerd
//
// This file provides utilities to support V8 Fast API Calls, which allow optimized
// method calls from JavaScript to C++ without going through the V8 API. Fast API
// Calls perform type checks in the compiler instead of on the embedder side and
// are subject to strict limitations - they cannot allocate on the JS heap or
// trigger JS execution.
//
// The implementation provides concepts and helpers to determine which types and methods
// are compatible with Fast API, handling both primitive types that can be passed directly
// and wrapped objects that require conversion between JavaScript and C++.

#include <v8-fast-api-calls.h>
#include <v8-local-handle.h>
#include <v8-value.h>

#include <kj/async.h>
#include <kj/common.h>
#include <kj/string.h>

namespace workerd::jsg {

class ByteString;
class DOMString;
class Lock;
class USVString;
template <typename T>
class Promise;

// Update this list whenever a new string type is added.
// TODO(soon): Merge this with webidl::isStringType once NonCoercible is supported.
template <typename T>
concept StringLike =
    kj::isSameType<kj::String, T>() || kj::isSameType<kj::ArrayPtr<const char>, T>() ||
    kj::isSameType<kj::Array<const char>, T>() || kj::isSameType<ByteString, T>() ||
    kj::isSameType<USVString, T>() || kj::isSameType<DOMString, T>();

template <typename T>
constexpr bool isFunctionCallbackInfo = false;

template <typename T>
constexpr bool isFunctionCallbackInfo<v8::FunctionCallbackInfo<T>> = true;

template <typename T>
constexpr bool isKjPromise = false;

template <typename T>
constexpr bool isKjPromise<kj::Promise<T>> = true;

template <typename T>
constexpr bool isJsgPromise = false;

template <typename T>
constexpr bool isJsgPromise<jsg::Promise<T>> = true;

// These types are passed by fast api as is and do not require wrapping/unwrapping.
template <typename T>
concept FastApiPrimitive = kj::isSameType<T, void>() || kj::isSameType<T, bool>() ||
    kj::isSameType<T, int32_t>() || kj::isSameType<T, int64_t>() || kj::isSameType<T, uint32_t>() ||
    kj::isSameType<T, uint64_t>() || kj::isSameType<T, float>() || kj::isSameType<T, double>();

// Helper to determine if a type can be used as a parameter in V8 Fast API
template <typename T>
concept FastApiParam = !isFunctionCallbackInfo<kj::RemoveConst<kj::Decay<T>>> &&
    !isKjPromise<kj::RemoveConst<kj::Decay<T>>> && !isJsgPromise<kj::RemoveConst<kj::Decay<T>>>;

// Helper to determine if a type can be used as a return value in a V8 Fast API
template <typename T>
concept FastApiReturnParam = FastApiPrimitive<T>;

// Helper to determine if a method can be used with V8 fast API
template <typename Ret, typename... Args>
concept FastApiMethod = FastApiReturnParam<Ret> && (FastApiParam<Args> && ...);

// Helper to determine if a method pointer type is compatible with Fast API
template <typename Method>
constexpr bool isFastApiCompatible = false;

// Specialization for non-static member functions
template <typename Class, typename Ret, typename... Args>
constexpr bool isFastApiCompatible<Ret (Class::*)(Args...)> = FastApiMethod<Ret, Args...>;

// Specialization for const non-static member functions
template <typename Class, typename Ret, typename... Args>
constexpr bool isFastApiCompatible<Ret (Class::*)(Args...) const> = FastApiMethod<Ret, Args...>;

template <typename Class, typename Ret, typename... Args>
constexpr bool isFastApiCompatible<Ret (Class::*)(jsg::Lock&, Args...)> =
    FastApiMethod<Ret, Args...>;

template <typename Class, typename Ret, typename... Args>
constexpr bool isFastApiCompatible<Ret (Class::*)(jsg::Lock&, Args...) const> =
    FastApiMethod<Ret, Args...>;

// Specialization for static functions
template <typename Ret, typename... Args>
constexpr bool isFastApiCompatible<Ret(Args...)> = FastApiMethod<Ret, Args...>;

template <typename Ret, typename... Args>
constexpr bool isFastApiCompatible<Ret(jsg::Lock&, Args...)> = FastApiMethod<Ret, Args...>;

template <typename T>
struct FastApiJSGToV8 {
  using value = v8::Local<v8::Value>;
};

template <typename T>
  requires StringLike<kj::RemoveConst<kj::Decay<T>>>
struct FastApiJSGToV8<T> {
  using value = const v8::FastOneByteString&;
};

template <typename T>
  requires FastApiPrimitive<kj::RemoveConst<kj::Decay<T>>>
struct FastApiJSGToV8<T> {
  using value = kj::RemoveConst<kj::Decay<T>>;
};

}  // namespace workerd::jsg
