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

  EvalResult eval(::rust::Str code) const;
  void set_global(::rust::Str name, ::workerd::rust::jsg::Local value) const;

  v8::Isolate* v8Isolate;
  v8::Global<v8::Context> v8Context;
};

// Testing harness that provides a simple V8 isolate for Rust JSG testing
class TestHarness {
 public:
  // Use create_test_harness() instead - it ensures proper V8 stack scope
  TestHarness(::workerd::jsg::V8StackScope& stackScope);

  // Runs a callback within a proper V8 context and stack scope
  // The callback receives the data pointer, isolate and a context
  void run_in_context(size_t data, ::rust::Fn<void(size_t, Isolate*, EvalContext&)> callback) const;

 private:
  mutable kj::Own<TestIsolate> isolate;
  mutable v8::Locker locker;
  mutable v8::Isolate::Scope isolateScope;
  mutable ::rust::Box<::workerd::rust::jsg::Realm> realm;
};

kj::Own<TestHarness> create_test_harness();

}  // namespace rust::jsg_test

}  // namespace workerd
