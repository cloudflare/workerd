// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/rust/jsg/ffi.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <kj-rs/kj-rs.h>
#include <rust/cxx.h>
#include <v8.h>

#include <kj/function.h>
#include <kj/memory.h>

namespace workerd {
class TestIsolate;

namespace rust::jsg_test {

using Isolate = v8::Isolate;

struct EvalResult;

class EvalContext {
 public:
  EvalContext(v8::Isolate* isolate, v8::Local<v8::Context> context);

  EvalResult eval(::rust::Str code, kj::Maybe<::rust::Str> resourceName) const;
  void set_global(::rust::Str name, ::workerd::rust::jsg::Local value) const;

  v8::Isolate* v8Isolate;
  v8::Global<v8::Context> v8Context;
};

// Forward-declared; defined by the CXX-generated lib.rs.h header.
enum class FastApi : uint8_t;

// Testing harness that provides a simple V8 isolate for Rust JSG testing
class TestHarness {
 public:
  // Use create_test_harness() instead - it ensures proper V8 stack scope
  TestHarness(::workerd::jsg::V8StackScope& stackScope,
      ::rust::Slice<const uint8_t> flags,
      FastApi fastApi);

  // Runs a callback within a proper V8 context and stack scope
  // The callback receives the data pointer, isolate and a context
  void run_in_context(size_t data, ::rust::Fn<void(size_t, Isolate*, EvalContext&)> callback) const;

 private:
  mutable kj::Own<TestIsolate> isolate;
  mutable v8::Locker locker;
  mutable v8::Isolate::Scope isolateScope;
  mutable ::rust::Box<::workerd::rust::jsg::Realm> realm;
};

// `flags` is a canonical `CompatibilityFlags` message. It sets the Rust realm's flags and, as
// `Worker::Isolate` does, the isolate-level JSG switches derived from them.
kj::Own<TestHarness> create_test_harness(::rust::Slice<const uint8_t> flags, FastApi fast_api);

// Forward-declared; defined by the CXX-generated lib.rs.h header.
enum class GcType : uint8_t;

// Triggers garbage collection for testing purposes.
void request_gc(Isolate* isolate, GcType gc_type);

// Calls `callback` with the isolate unlocked, then locks it again.
void run_unlocked(Isolate* isolate, size_t data, ::rust::Fn<void(size_t)> callback);

// v8::HeapStatistics::used_global_handles_size() of the isolate.
size_t used_global_handles_size(Isolate* isolate);

// Creates a V8 object with the C++ WORKERD_WRAPPABLE_TAG set in its internal fields.
// Used to test that Rust unwrap correctly rejects non-Rust wrappable objects.
::workerd::rust::jsg::Local create_cpp_tagged_object(Isolate* isolate);

// Returns an empty v8::MaybeLocal, to test how it reaches Rust.
::workerd::rust::jsg::MaybeLocal empty_maybe_local();

// Returns whether `value` is empty as a v8::MaybeLocal, to test what Rust passes for `None`.
bool cpp_sees_empty_maybe_local(::workerd::rust::jsg::MaybeLocal value);

}  // namespace rust::jsg_test

}  // namespace workerd
