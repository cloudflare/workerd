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

#include <workerd/jsg/web-idl.h>

#include <v8-fast-api-calls.h>
#include <v8-local-handle.h>
#include <v8-value.h>

#include <kj/common.h>

namespace workerd::jsg {

template <typename T>
constexpr bool isFunctionCallbackInfo = false;

template <typename T>
constexpr bool isFunctionCallbackInfo<v8::FunctionCallbackInfo<T>> = true;

// These types are passed by fast api as is and do not require wrapping/unwrapping.
template <typename T>
concept FastApiPrimitive = kj::isSameType<T, void>() || kj::isSameType<T, bool>() ||
    kj::isSameType<T, int32_t>() || kj::isSameType<T, int64_t>() || kj::isSameType<T, uint32_t>() ||
    kj::isSameType<T, uint64_t>() || kj::isSameType<T, float>() || kj::isSameType<T, double>();

// Helper to determine if a type can be used as a parameter in V8 Fast API
template <typename TypeWrapper, typename T>
concept FastApiParam = !isFunctionCallbackInfo<kj::RemoveConst<kj::Decay<T>>>;

// Helper to determine if a type can be used as a return value in a V8 Fast API
template <typename T>
concept FastApiReturnParam = FastApiPrimitive<T>;

// Helper to determine if a method can be used with V8 fast API
template <typename TypeWrapper, typename Ret, typename... Args>
concept FastApiMethod = FastApiReturnParam<Ret> && (FastApiParam<TypeWrapper, Args> && ...);

// Helper to determine if a method pointer type is compatible with Fast API
template <typename TypeWrapper, typename Method>
constexpr bool isFastMethodCompatible = false;

// Specialization for non-static member functions
template <typename TypeWrapper, typename Class, typename Ret, typename... Args>
constexpr bool isFastMethodCompatible<TypeWrapper, Ret (Class::*)(Args...)> =
    FastApiMethod<TypeWrapper, Ret, Args...>;

// Specialization for const non-static member functions
template <typename TypeWrapper, typename Class, typename Ret, typename... Args>
constexpr bool isFastMethodCompatible<TypeWrapper, Ret (Class::*)(Args...) const> =
    FastApiMethod<TypeWrapper, Ret, Args...>;

template <typename TypeWrapper, typename Class, typename Ret, typename... Args>
constexpr bool isFastMethodCompatible<TypeWrapper, Ret (Class::*)(jsg::Lock&, Args...)> =
    FastApiMethod<TypeWrapper, Ret, Args...>;

template <typename TypeWrapper, typename Class, typename Ret, typename... Args>
constexpr bool isFastMethodCompatible<TypeWrapper, Ret (Class::*)(jsg::Lock&, Args...) const> =
    FastApiMethod<TypeWrapper, Ret, Args...>;

// Specialization for static functions
template <typename TypeWrapper, typename Ret, typename... Args>
constexpr bool isFastMethodCompatible<TypeWrapper, Ret(Args...)> =
    FastApiMethod<TypeWrapper, Ret, Args...>;

template <typename TypeWrapper, typename Ret, typename... Args>
constexpr bool isFastMethodCompatible<TypeWrapper, Ret(jsg::Lock&, Args...)> =
    FastApiMethod<TypeWrapper, Ret, Args...>;

template <typename TypeWrapper, typename T>
struct FastApiJSGToV8 {
  // We allow every type to be passed into v8 fast api using v8::Local<v8::Value>
  // conversion.
  using value = v8::Local<v8::Value>;
};

template <typename TypeWrapper, typename T>
  requires FastApiPrimitive<kj::RemoveConst<kj::Decay<T>>>
struct FastApiJSGToV8<TypeWrapper, T> {
  using value = kj::RemoveConst<kj::Decay<T>>;
};

}  // namespace workerd::jsg
