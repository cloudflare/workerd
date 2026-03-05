// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// INTERNAL IMPLEMENTATION FILE

#include <v8-function-callback.h>

#include <kj/tuple.h>

namespace workerd::jsg {

class Lock;

template <typename T>
struct ArgumentIndexes_;
template <typename T, typename Ret, typename... Args>
struct ArgumentIndexes_<Ret (T::*)(Args...)> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename T, typename Ret, typename... Args>
struct ArgumentIndexes_<Ret (T::*)(Lock&, Args...)> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename T, typename Ret, typename... Args>
struct ArgumentIndexes_<Ret (T::*)(const v8::FunctionCallbackInfo<v8::Value>&, Args...)> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename T, typename Ret, typename... Args>
struct ArgumentIndexes_<Ret (T::*)(Args...) const> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename T, typename Ret, typename... Args>
struct ArgumentIndexes_<Ret (T::*)(Lock&, Args...) const> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename T, typename Ret, typename... Args>
struct ArgumentIndexes_<Ret (T::*)(const v8::FunctionCallbackInfo<v8::Value>&, Args...) const> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename Ret, typename... Args>
struct ArgumentIndexes_<Ret(Args...)> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename Ret, typename... Args>
struct ArgumentIndexes_<Ret(Lock&, Args...)> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename Ret, typename... Args>
struct ArgumentIndexes_<Ret(const v8::FunctionCallbackInfo<v8::Value>&, Args...)> {
  using Indexes = kj::_::MakeIndexes<sizeof...(Args)>;
};
template <typename T>
using ArgumentIndexes = ArgumentIndexes_<T>::Indexes;
// ArgumentIndexes<SomeMethodType> expands to kj::_::Indexes<0, 1, 2, 3, ..., n-1>, where n is the
// number of arguments to the method, not counting the magic Lock or FunctionCallbackInfo parameter
// (if any).

// =======================================================================================
// requiredArgumentCount<T> — counts leading required JS-visible arguments.
//
// Used by resource.h to set the Web IDL .length property on functions.
//
// Argument filtering is split across two layers for dependency reasons:
//
//  1. StripMagicParam_ (here in meta.h) removes the leading Lock& or
//     FunctionCallbackInfo& parameter.  These are C++/V8 plumbing that always
//     appear first and are never JS-visible.  meta.h can handle them because
//     it only needs the v8 forward declarations it already includes.
//
//  2. RequiredArgCount_ (in web-idl.h) skips TypeHandler<T> and Arguments<T>
//     when counting.  These types can appear at any position in the parameter
//     list and are "invisible" to JS callers, but they are JSG-specific types
//     that are not available in meta.h's include graph.  web-idl.h has the
//     full JSG type system visible, so the counting logic lives there.

// Lightweight type list; kj::Tuple is an alias template and cannot be partially specialized.
template <typename... Ts>
struct TypeList {};

namespace detail {

// Phase 1: Normalize member-function-pointer or free-function type to Ret(Args...).
template <typename T>
struct NormalizeFunc_;
template <typename C, typename R, typename... A>
struct NormalizeFunc_<R (C::*)(A...)> {
  using type = R(A...);
};
template <typename C, typename R, typename... A>
struct NormalizeFunc_<R (C::*)(A...) const> {
  using type = R(A...);
};
template <typename R, typename... A>
struct NormalizeFunc_<R(A...)> {
  using type = R(A...);
};

// Phase 2: Strip leading Lock& / FunctionCallbackInfo& and yield the JS-visible args.
// See the comment above for why this is separate from RequiredArgCount_.
template <typename T>
struct StripMagicParam_;
template <typename R, typename... A>
struct StripMagicParam_<R(A...)> {
  using Args = TypeList<A...>;
};
template <typename R, typename... A>
struct StripMagicParam_<R(Lock&, A...)> {
  using Args = TypeList<A...>;
};
template <typename R, typename... A>
struct StripMagicParam_<R(const v8::FunctionCallbackInfo<v8::Value>&, A...)> {
  using Args = TypeList<A...>;
};

template <typename T>
using MethodArgs = StripMagicParam_<typename NormalizeFunc_<T>::type>;

// Forward declaration — specialized in web-idl.h where the full JSG type system is visible.
template <typename ArgsList>
struct RequiredArgCount_;

}  // namespace detail

// Per Web IDL, the .length of a function is the number of leading required arguments.
// The actual counting logic lives in web-idl.h.
template <typename T>
inline constexpr int requiredArgumentCount =
    detail::RequiredArgCount_<typename detail::MethodArgs<T>::Args>::value;

}  // namespace workerd::jsg
