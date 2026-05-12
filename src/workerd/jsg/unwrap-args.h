// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// INTERNAL IMPLEMENTATION FILE
//
// Deterministic left-to-right argument unwrapping for JSG-generated V8
// callbacks.
//
// Background
// ----------
// JSG-generated method/constructor/static-method/functor callbacks need to
// convert each `v8::Local<v8::Value>` argument in a `FunctionCallbackInfo`
// into a typed C++ value via `TypeWrapper::unwrap<Args>(...)`.  The natural
// way to write this is a pack expansion inside the function-call argument
// list:
//
//     (self.*method)(lock,
//         wrapper.template unwrap<Args>(lock, context, args, indexes, ...)...);
//
// Per the C++ standard ([expr.call]), the order in which a function call's
// argument expressions are evaluated is *unsequenced*.  Different toolchains
// make different choices: Clang and GCC evaluate left-to-right on Linux;
// MSVC evaluates right-to-left on Windows.  Since `unwrap` can fire
// user-defined JS code (e.g. `toString`, getters, `Symbol.iterator`), the
// order in which the unwraps run is observable from JavaScript.  This
// contradicts Web IDL's requirement that operation arguments be evaluated
// left-to-right.
//
// Approach
// --------
// `UnwrappedArgs<Indexes<I...>, Args...>` inherits from
// `UnwrappedArg<I, Args>...`.  Each `UnwrappedArg` constructor invokes a
// caller-supplied callable to produce its value.  Per the C++ standard
// ([class.base.init]), non-virtual base subobjects are initialized in their
// declaration order — which, for a pack-expanded base list, is
// left-to-right.  This is the same trick `kj::Tuple` uses to initialize its
// `TupleElement<i, T>` bases in order, generalised in two ways: (1) the
// per-element constructor invokes a user-supplied callable to produce its
// value, rather than receiving an already-evaluated value; (2) for
// rvalue-reference parameters (e.g. `JsgStruct&&` arguments for move-in),
// `RemoveRvalueRef<T>` is used as the storage type so the produced value
// is owned by `UnwrappedArg` and forwarded back as an rvalue reference on
// extraction.  `kj::Tuple<T&&>` instead declares a dangling `T&&` member
// with no extension-of-lifetime, which would clash with the
// produce-and-own semantics here.
//
// Usage
// -----
//     auto unwrapped = _::unwrapArgs<Args...>(wrapper, lock, context, args,
//         []<size_t i>() { return TypeErrorContext::methodArgument(typeid(T), methodName, i); });
//     (self.*method)(lock, kj::mv(unwrapped).template take<indexes>()...);
//
// The second pack expansion (`take<indexes>()...`) is safe: each `take`
// call is a `kj::fwd` of an already-initialized member, with no side
// effects, so the outer call's argument-evaluation order is irrelevant.

#include <workerd/jsg/util.h>  // for RemoveRvalueRef

#include <kj/common.h>
#include <kj/tuple.h>  // for kj::_::Indexes, kj::_::TypeByIndex

#include <cstddef>

namespace workerd::jsg::_ {  // private

// Holds the unwrapped value for argument slot `Index` of an
// `UnwrappedArgs<Indexes<...>, Args...>`.  `U` is the original method
// parameter type; we store `RemoveRvalueRef<U>` which is exactly what
// `TypeWrapper::unwrap<U>(...)` returns.  This preserves lvalue-reference
// parameter types (e.g. `Lock&`, `TypeHandler<T>&`) — reference members
// bind during mem-init — and stores other parameter types by value.
template <size_t Index, typename U>
struct UnwrappedArg {
  using Type = RemoveRvalueRef<U>;
  Type value;

  template <typename Unwrap>
  explicit UnwrappedArg(Unwrap& unwrap): value(unwrap.template operator()<Index, U>()) {}
};

template <typename Indexes, typename... Args>
struct UnwrappedArgs;

template <size_t... I, typename... Args>
struct UnwrappedArgs<kj::_::Indexes<I...>, Args...>: UnwrappedArg<I, Args>... {
  // Non-virtual base subobjects are initialized in declaration order
  // ([class.base.init]), which — because the base list is a pack expansion
  // of `UnwrappedArg<I, Args>` — is left-to-right.  This is the guarantee
  // that fixes the unsequenced-function-argument-evaluation hazard at the
  // original JSG call sites.
  //
  // The `unwrap` callable is taken by value so its lifetime spans the
  // construction of all base subobjects.  Each `UnwrappedArg` ctor
  // receives it by lvalue reference.
  template <typename Unwrap>
  explicit UnwrappedArgs(Unwrap unwrap): UnwrappedArg<I, Args>(unwrap)... {}

  KJ_DISALLOW_COPY_AND_MOVE(UnwrappedArgs);

  // Forward the value out of the Idx'th argument slot.  Intended to be
  // called exactly once per slot, on an rvalue `UnwrappedArgs`.
  //
  // We use `kj::fwd<T>` (a.k.a. `std::forward<T>`) rather than `kj::mv`
  // so that reference-typed parameters (e.g. `Lock&`, `TypeHandler<T>&`)
  // come out as lvalue references that bind to non-const lvalue-ref
  // parameters.  Reference collapsing on `kj::fwd<T>(value)`:
  //   - T = ValueType   → rvalue ref T&&  (move into value param)
  //   - T = T&          → lvalue ref T&   (binds to lvalue-ref param)
  //   - T = T&&         → rvalue ref T&&  (binds to rvalue-ref param)
  template <size_t Idx>
  decltype(auto) take() && {
    using T = kj::_::TypeByIndex<Idx, Args...>;
    return kj::fwd<T>(static_cast<UnwrappedArg<Idx, T>&>(*this).value);
  }
};

// Convenience factory that constructs an UnwrappedArgs, deducing the
// index pack internally from sizeof...(Args).  The error-context factory
// `makeEC` is invoked once per slot with the slot index as a compile-time
// template parameter, and should return a TypeErrorContext describing that
// argument position.
//
// C++17 guaranteed copy elision allows returning the non-movable
// UnwrappedArgs by prvalue.

// Implementation: needs the indexes as a deduced pack to construct the
// UnwrappedArgs return type.
template <typename... Args, size_t... indexes, typename TypeWrapper, typename MakeErrorContext>
UnwrappedArgs<kj::_::Indexes<indexes...>, Args...> unwrapArgsImpl(kj::_::Indexes<indexes...>,
    TypeWrapper& wrapper,
    Lock& js,
    v8::Local<v8::Context> context,
    const v8::FunctionCallbackInfo<v8::Value>& args,
    MakeErrorContext& makeErrorContext) {
  auto doUnwrap = [&]<size_t I, typename V>() -> decltype(auto) {
    return wrapper.template unwrap<V>(
        js, context, args, I, makeErrorContext.template operator()<I>());
  };
  return UnwrappedArgs<kj::_::Indexes<indexes...>, Args...>(doUnwrap);
}

// Public entry point — callers only need to supply `Args...`; the index
// pack is reconstructed from `sizeof...(Args)`.
template <typename... Args, typename TypeWrapper, typename MakeErrorContext>
auto unwrapArgs(TypeWrapper& wrapper,
    Lock& js,
    v8::Local<v8::Context> context,
    const v8::FunctionCallbackInfo<v8::Value>& args,
    MakeErrorContext makeErrorContext) {
  return unwrapArgsImpl<Args...>(
      kj::_::MakeIndexes<sizeof...(Args)>{}, wrapper, js, context, args, makeErrorContext);
}

}  // namespace workerd::jsg::_
