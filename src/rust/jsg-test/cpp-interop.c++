// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "cpp-interop.h"

#include <workerd/rust/jsg-test/cpp_interop.rs.h>

namespace workerd::cpp_interop_test {

namespace {
uint32_t holdersDestroyed = 0;
}  // namespace

kj::Maybe<bool> HostTag::isInstance(v8::Isolate* isolate, jsg_cpp::Local value) {
  return ::workerd::rust::jsg_test::host_is(isolate, value);
}

kj::String TestCppGreeter::greet(::workerd::jsg::Lock& js, kj::String name) {
  return kj::str(greeting, ", ", name);
}

void TestCppGreeter::failWithTypeError() {
  JSG_FAIL_REQUIRE(TypeError, "thrown by C++ as a kj::Exception");
}

void TestCppGreeter::failWithJsError(::workerd::jsg::Lock& js) {
  js.throwException(js.rangeError("thrown by C++ as a JavaScript exception"));
}

TestCppHolder::~TestCppHolder() noexcept(false) {
  ++holdersDestroyed;
}

::workerd::jsg::Optional<HostRef> TestCppHolder::getHeld(::workerd::jsg::Lock& js) {
  return held.map([&](HostRef& host) { return host.addRef(js); });
}

kj::String TestCppHolder::describeHeld(::workerd::jsg::Lock& js) {
  auto& host = JSG_REQUIRE_NONNULL(held, Error, "nothing is held");
  v8::Local<v8::Value> description;
  if (!::workerd::rust::jsg::maybe_local_from_ffi<v8::Value>(
          ::workerd::rust::jsg_test::host_describe(js.v8Isolate, host.toFfi(js)))
           .ToLocal(&description)) {
    throw ::workerd::jsg::JsExceptionThrown();
  }
  return js.toString(description);
}

kj::StringPtr TestCppHolder::which(kj::OneOf<HostRef, ::workerd::jsg::Ref<TestCppGreeter>> value) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(host, HostRef) {
      return "Host"_kj;
    }
    KJ_CASE_ONEOF(greeter, ::workerd::jsg::Ref<TestCppGreeter>) {
      return "TestCppGreeter"_kj;
    }
  }
  KJ_UNREACHABLE;
}

uint32_t holdersDestroyedForTest() {
  return holdersDestroyed;
}

}  // namespace workerd::cpp_interop_test

namespace workerd::rust::jsg_test {

using ::workerd::cpp_interop_test::HostRef;

namespace {
bool inRequest = false;

// Wraps a new C++ resource, as C++ code returns one to JavaScript.
template <typename T>
jsg_cpp::MaybeLocal wrapNew(v8::Isolate* isolate, ::workerd::jsg::Ref<T> value) {
  return jsg_cpp::cppRefWrap<T>(isolate, jsg_cpp::CppRef(kj::mv(value)));
}
}  // namespace

kj::Maybe<kj::Own<jsg_cpp::CppRef>> test_cpp_greeter_try_unwrap(
    v8::Isolate* isolate, jsg_cpp::Local value) {
  return jsg_cpp::cppRefTryUnwrap<TestCppGreeter>(isolate, kj::mv(value));
}

jsg_cpp::MaybeLocal test_cpp_greeter_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref) {
  return jsg_cpp::cppRefWrap<TestCppGreeter>(isolate, ref);
}

kj::Maybe<kj::Own<jsg_cpp::CppRef>> test_cpp_base_try_unwrap(
    v8::Isolate* isolate, jsg_cpp::Local value) {
  return jsg_cpp::cppRefTryUnwrap<TestCppBase>(isolate, kj::mv(value));
}

jsg_cpp::MaybeLocal test_cpp_base_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref) {
  return jsg_cpp::cppRefWrap<TestCppBase>(isolate, ref);
}

kj::Maybe<kj::Own<jsg_cpp::CppRef>> test_cpp_holder_try_unwrap(
    v8::Isolate* isolate, jsg_cpp::Local value) {
  return jsg_cpp::cppRefTryUnwrap<TestCppHolder>(isolate, kj::mv(value));
}

jsg_cpp::MaybeLocal test_cpp_holder_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref) {
  return jsg_cpp::cppRefWrap<TestCppHolder>(isolate, ref);
}

// Rust's `&T` of an opaque type is interior-mutable, so the shims may call non-const methods.
bool test_cpp_greeter_greet(
    v8::Isolate* isolate, const TestCppGreeter& greeter, ::rust::Str name, ::rust::String& result) {
  return jsg_cpp::callCpp(isolate, [&](::workerd::jsg::Lock& js) {
    auto greeting = const_cast<TestCppGreeter&>(greeter).greet(js, kj::str(name));
    result = ::rust::String(greeting.cStr(), greeting.size());
  });
}

bool test_cpp_greeter_fail_with_type_error(v8::Isolate* isolate, const TestCppGreeter& greeter) {
  return jsg_cpp::callCpp(isolate,
      [&](::workerd::jsg::Lock&) { const_cast<TestCppGreeter&>(greeter).failWithTypeError(); });
}

bool test_cpp_greeter_fail_with_js_error(v8::Isolate* isolate, const TestCppGreeter& greeter) {
  return jsg_cpp::callCpp(isolate,
      [&](::workerd::jsg::Lock& js) { const_cast<TestCppGreeter&>(greeter).failWithJsError(js); });
}

bool test_cpp_base_kind(v8::Isolate* isolate, const TestCppBase& base, ::rust::String& result) {
  return jsg_cpp::callCpp(isolate, [&](::workerd::jsg::Lock&) {
    auto kind = const_cast<TestCppBase&>(base).kind();
    result = ::rust::String(kind.begin(), kind.size());
  });
}

bool test_cpp_current_base(v8::Isolate* isolate, kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result) {
  return jsg_cpp::callCppForRef<TestCppBase>(isolate, result,
      [](::workerd::jsg::Lock& js) -> ::workerd::jsg::Optional<::workerd::jsg::Ref<TestCppBase>> {
    if (!inRequest) return kj::none;
    return ::workerd::jsg::Ref<TestCppBase>(js.alloc<TestCppDerived>());
  });
}

void test_cpp_set_in_request(bool value) {
  inRequest = value;
}

uint32_t test_cpp_holders_destroyed() {
  return ::workerd::cpp_interop_test::holdersDestroyedForTest();
}

jsg_cpp::MaybeLocal test_cpp_greeter_new(v8::Isolate* isolate, ::rust::Str greeting) {
  auto& js = ::workerd::jsg::Lock::from(isolate);
  return wrapNew(isolate, js.alloc<TestCppGreeter>(kj::str(greeting)));
}

jsg_cpp::MaybeLocal test_cpp_derived_new(v8::Isolate* isolate) {
  auto& js = ::workerd::jsg::Lock::from(isolate);
  return wrapNew(isolate, js.alloc<TestCppDerived>());
}

jsg_cpp::MaybeLocal test_cpp_holder_new(v8::Isolate* isolate) {
  auto& js = ::workerd::jsg::Lock::from(isolate);
  return wrapNew(isolate, js.alloc<TestCppHolder>());
}

jsg_cpp::MaybeLocal test_cpp_holder_new_holding_host(v8::Isolate* isolate, ::rust::Str label) {
  v8::MaybeLocal<v8::Value> result;
  jsg_cpp::callCpp(isolate, [&](::workerd::jsg::Lock& js) {
    auto host = HostRef::fromFfi(js, host_new(isolate, label));
    auto holder = js.alloc<TestCppHolder>();
    holder->hold(kj::mv(host));
    auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<::workerd::jsg::Ref<TestCppHolder>>());
    result = handler.wrap(js, kj::mv(holder));
  });
  return ::workerd::rust::jsg::maybe_local_to_ffi(result);
}

}  // namespace workerd::rust::jsg_test
