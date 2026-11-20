// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// JavaScript functions over C++ callbacks, for tests that hand JavaScript a function to call.

#include <workerd/jsg/jsg.h>

#include <v8-external.h>
#include <v8-function.h>

namespace workerd {

// A JavaScript function over `callback`, with no data.
inline v8::Local<v8::Function> makeFunction(jsg::Lock& js, v8::FunctionCallback callback) {
  return jsg::check(v8::Function::New(js.v8Context(), callback));
}

// A JavaScript function over `callback` whose data points at `state`, which must outlive the
// function; stateOf() reads it back.
template <typename State>
v8::Local<v8::Function> makeFunction(jsg::Lock& js, State& state, v8::FunctionCallback callback) {
  return jsg::check(v8::Function::New(js.v8Context(), callback,
      v8::External::New(js.v8Isolate, &state, v8::kExternalPointerTypeTagDefault)));
}

template <typename State>
State& stateOf(const v8::FunctionCallbackInfo<v8::Value>& info) {
  return *static_cast<State*>(
      info.DataV2().As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
}

// The C++ side of a function that needs nothing but the lock and the call.
using LiftedCallback = v8::Local<v8::Value> (*)(
    jsg::Lock&, const v8::FunctionCallbackInfo<v8::Value>&);

// A JavaScript function that calls `callback` under jsg::liftKj() and returns what it returns.
inline v8::Local<v8::Function> makeLiftedFunction(jsg::Lock& js, LiftedCallback callback) {
  return jsg::check(v8::Function::New(js.v8Context(),
      [](const v8::FunctionCallbackInfo<v8::Value>& info) {
    jsg::liftKj(info, [&]() {
      auto callback = reinterpret_cast<LiftedCallback>(
          info.DataV2().As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
      return callback(jsg::Lock::from(info.GetIsolate()), info);
    });
  },
      v8::External::New(
          js.v8Isolate, reinterpret_cast<void*>(callback), v8::kExternalPointerTypeTagDefault)));
}

}  // namespace workerd
