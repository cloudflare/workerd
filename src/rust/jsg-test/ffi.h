#pragma once

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

// Testing harness that provides a simple V8 isolate for Rust JSG testing
class TestHarness {
 public:
  TestHarness();

  // Runs a callback within a proper V8 context and stack scope
  void run_in_context(::rust::Fn<void(Isolate*)> callback) const;

 private:
  mutable kj::Own<TestIsolate> isolate;
  mutable v8::Locker locker;
  mutable v8::Isolate::Scope isolateScope;
  mutable ::rust::Box<::workerd::rust::jsg::Realm> realm;
};

kj::Own<TestHarness> create_test_harness();

// Triggers a full garbage collection for testing purposes.
void request_gc(Isolate* isolate);

}  // namespace rust::jsg_test

}  // namespace workerd
