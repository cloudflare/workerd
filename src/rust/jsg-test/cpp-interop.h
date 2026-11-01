// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// C++ JSG resources for testing the interop of `jsg_cpp` (src/rust/jsg-cpp), and the per-type and
// per-method shims through which Rust uses them (cpp_interop.rs). The harness's isolate registers
// them, with jsg_cpp::RustResourceWrapper.

#include <workerd/jsg/jsg.h>
#include <workerd/rust/jsg-cpp/jsg-cpp.h>

#include <rust/cxx.h>

// The classes are in a namespace of their own, outside `workerd::rust`, where `jsg` names C++
// JSG, as JSG's macros expect.
namespace workerd::cpp_interop_test {

namespace jsg_cpp = ::workerd::rust::jsg_cpp;

// Stands for the Rust resource `cpp_interop::Host` in C++ JSG signatures.
struct HostTag {
  static constexpr const char* NAME = "Host";
  static kj::Maybe<bool> isInstance(v8::Isolate* isolate, jsg_cpp::Local value);
};
using HostRef = jsg_cpp::RustRef<HostTag>;

// A C++ resource whose methods Rust calls.
class TestCppGreeter final: public jsg::Object {
 public:
  explicit TestCppGreeter(kj::String greeting): greeting(kj::mv(greeting)) {}

  kj::String greet(jsg::Lock& js, kj::String name);
  // Fails as C++ API code usually does: with a kj::Exception that JSG_REQUIRE made.
  void failWithTypeError();
  // Throws a JavaScript exception, as C++ code that called into JavaScript passes one on.
  void failWithJsError(jsg::Lock& js);

  JSG_RESOURCE_TYPE(TestCppGreeter) {
    JSG_METHOD(greet);
  }

 private:
  kj::String greeting;
};

// A C++ resource with a subclass, as `CacheContext` has.
class TestCppBase: public jsg::Object {
 public:
  virtual kj::StringPtr kind() {
    return "base"_kj;
  }

  JSG_RESOURCE_TYPE(TestCppBase) {
    JSG_METHOD(kind);
  }
};

class TestCppDerived final: public TestCppBase {
 public:
  kj::StringPtr kind() override {
    return "derived"_kj;
  }
  kj::StringPtr extra() {
    return "only on the subclass"_kj;
  }

  JSG_RESOURCE_TYPE(TestCppDerived) {
    JSG_INHERIT(TestCppBase);
    JSG_METHOD(extra);
  }
};

// A C++ resource that holds, passes and type-checks a Rust resource, as C++ code does with
// `ExecutionContext`.
class TestCppHolder final: public jsg::Object {
 public:
  ~TestCppHolder() noexcept(false);

  void hold(HostRef host) {
    held = kj::mv(host);
  }
  // The held resource, the same JavaScript object `hold()` was given.
  jsg::Optional<HostRef> getHeld(jsg::Lock& js);
  // Calls a method of the held Rust resource.
  kj::String describeHeld(jsg::Lock& js);
  // A union of a Rust and a C++ resource, as `ExecutionContextOrState` is.
  kj::StringPtr which(kj::OneOf<HostRef, jsg::Ref<TestCppGreeter>> value);

  JSG_RESOURCE_TYPE(TestCppHolder) {
    JSG_METHOD(hold);
    JSG_METHOD(getHeld);
    JSG_METHOD(describeHeld);
    JSG_METHOD(which);
  }

 private:
  kj::Maybe<HostRef> held;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(held);
  }
};

// How many TestCppHolders have been destroyed.
uint32_t holdersDestroyedForTest();

}  // namespace workerd::cpp_interop_test

namespace workerd::rust::jsg_test {

using ::workerd::cpp_interop_test::TestCppBase;
using ::workerd::cpp_interop_test::TestCppDerived;
using ::workerd::cpp_interop_test::TestCppGreeter;
using ::workerd::cpp_interop_test::TestCppHolder;

// Per-type shims, as a crate binding C++ resources declares them.
kj::Maybe<kj::Own<jsg_cpp::CppRef>> test_cpp_greeter_try_unwrap(
    v8::Isolate* isolate, jsg_cpp::Local value);
jsg_cpp::MaybeLocal test_cpp_greeter_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref);
kj::Maybe<kj::Own<jsg_cpp::CppRef>> test_cpp_base_try_unwrap(
    v8::Isolate* isolate, jsg_cpp::Local value);
jsg_cpp::MaybeLocal test_cpp_base_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref);
kj::Maybe<kj::Own<jsg_cpp::CppRef>> test_cpp_holder_try_unwrap(
    v8::Isolate* isolate, jsg_cpp::Local value);
jsg_cpp::MaybeLocal test_cpp_holder_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref);

// Per-method shims. Each returns false with an exception pending if the C++ threw.
bool test_cpp_greeter_greet(
    v8::Isolate* isolate, const TestCppGreeter& greeter, ::rust::Str name, ::rust::String& result);
bool test_cpp_greeter_fail_with_type_error(v8::Isolate* isolate, const TestCppGreeter& greeter);
bool test_cpp_greeter_fail_with_js_error(v8::Isolate* isolate, const TestCppGreeter& greeter);
bool test_cpp_base_kind(v8::Isolate* isolate, const TestCppBase& base, ::rust::String& result);

// The base of the current simulated request, as `Worker::Api::getCtxCacheProperty()` returns the
// `CacheContext` of the current request: during one, a new TestCppDerived; otherwise none.
bool test_cpp_current_base(v8::Isolate* isolate, kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result);

// Test controls and constructors, called from Rust tests.
void test_cpp_set_in_request(bool inRequest);
uint32_t test_cpp_holders_destroyed();
jsg_cpp::MaybeLocal test_cpp_greeter_new(v8::Isolate* isolate, ::rust::Str greeting);
jsg_cpp::MaybeLocal test_cpp_derived_new(v8::Isolate* isolate);
jsg_cpp::MaybeLocal test_cpp_holder_new(v8::Isolate* isolate);
// Creates a Rust `Host` the way C++ creates an `ExecutionContext` for a request (through the
// crate's `extern "Rust"` constructor), checks its type, and has a new TestCppHolder hold it.
// Returns the holder's wrapper.
jsg_cpp::MaybeLocal test_cpp_holder_new_holding_host(v8::Isolate* isolate, ::rust::Str label);

}  // namespace workerd::rust::jsg_test
