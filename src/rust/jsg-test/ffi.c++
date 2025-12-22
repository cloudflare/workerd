#include "ffi.h"

#include <workerd/jsg/setup.h>
#include <workerd/rust/jsg-test/lib.rs.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/jsg/lib.rs.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <v8.h>

#include <kj/common.h>

using namespace kj_rs;

namespace {
// Enable predictable mode so RequestGarbageCollectionForTesting actually triggers GC.
// Without this, V8 may defer or skip the requested collection.
bool initPredictableMode = (workerd::setPredictableModeForTest(), true);
}  // namespace

namespace workerd {

struct TestContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(TestContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(TestIsolate, TestContext);

namespace rust::jsg_test {

// Lazy initialization of V8System to avoid static initialization order issues
static ::workerd::jsg::V8System& getV8System() {
  static ::workerd::jsg::V8System v8System;
  return v8System;
}

TestHarness::TestHarness(::workerd::jsg::V8StackScope&)
    : isolate(kj::heap<TestIsolate>(getV8System(), kj::heap<::workerd::jsg::IsolateObserver>())),
      locker(isolate->getIsolate()),
      isolateScope(isolate->getIsolate()),
      realm(::workerd::rust::jsg::realm_create(isolate->getIsolate())) {
  isolate->getIsolate()->SetData(::workerd::jsg::SetDataIndex::SET_DATA_RUST_REALM, &*realm);
}

kj::Own<TestHarness> create_test_harness() {
  return ::workerd::jsg::runInV8Stack(
      [](::workerd::jsg::V8StackScope& stackScope) { return kj::heap<TestHarness>(stackScope); });
}

EvalContext::EvalContext(v8::Isolate* isolate, v8::Local<v8::Context> context)
    : v8Isolate(isolate),
      v8Context(isolate, context) {}

void EvalContext::set_global(::rust::Str name, ::workerd::rust::jsg::Local value) const {
  auto ctx = v8Context.Get(v8Isolate);
  auto key = ::workerd::jsg::check(v8::String::NewFromUtf8(
      v8Isolate, name.data(), v8::NewStringType::kNormal, static_cast<int>(name.size())));
  auto v8Value = ::workerd::rust::jsg::local_from_ffi<v8::Value>(kj::mv(value));
  ::workerd::jsg::check(ctx->Global()->Set(ctx, key, v8Value));
}

EvalResult EvalContext::eval(::rust::Str code) const {
  EvalResult result;
  result.success = false;

  v8::Local<v8::Context> ctx = v8Context.Get(v8Isolate);

  v8::Local<v8::String> source = ::workerd::jsg::check(v8::String::NewFromUtf8(
      v8Isolate, code.data(), v8::NewStringType::kNormal, static_cast<int>(code.size())));

  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(ctx, source).ToLocal(&script)) {
    result.success = false;
    result.result_type = "CompileError";
    result.result_value = "Failed to compile script";
    return result;
  }

  v8::TryCatch catcher(v8Isolate);

  v8::Local<v8::Value> value;
  if (script->Run(ctx).ToLocal(&value)) {
    v8::String::Utf8Value type(v8Isolate, value->TypeOf(v8Isolate));
    v8::String::Utf8Value valueStr(v8Isolate, value);

    result.success = true;
    result.result_type = *type;
    result.result_value = *valueStr;
  } else if (catcher.HasCaught()) {
    v8::String::Utf8Value message(v8Isolate, catcher.Exception());

    result.success = false;
    result.result_type = "throws";
    result.result_value = *message ? *message : "Unknown error";
  } else {
    result.success = false;
    result.result_type = "error";
    result.result_value = "Returned empty handle but didn't throw exception";
  }

  return result;
}

void TestHarness::run_in_context(::rust::Fn<void(Isolate*, EvalContext&)> callback) const {
  isolate->runInLockScope([&](TestIsolate::Lock& lock) {
    auto context = lock.newContext<TestContext>();
    v8::Local<v8::Context> v8Context = context.getHandle(lock.v8Isolate);
    v8::Context::Scope contextScope(v8Context);

    EvalContext evalContext(lock.v8Isolate, v8Context);
    callback(lock.v8Isolate, evalContext);
  });
}

void TestHarness::set_global(::rust::Str name, ::workerd::rust::jsg::Local value) const {
  isolate->runInLockScope([&](TestIsolate::Lock& lock) {
    auto context = lock.newContext<TestContext>();
    v8::Local<v8::Context> v8Context = context.getHandle(lock.v8Isolate);
    v8::Context::Scope contextScope(v8Context);

    v8::Local<v8::String> key = ::workerd::jsg::check(v8::String::NewFromUtf8(
        lock.v8Isolate, name.data(), v8::NewStringType::kNormal, static_cast<int>(name.size())));
    v8::Local<v8::Value> v8Value = ::workerd::rust::jsg::local_from_ffi<v8::Value>(kj::mv(value));
    ::workerd::jsg::check(v8Context->Global()->Set(v8Context, key, v8Value));
  });
}

}  // namespace rust::jsg_test

}  // namespace workerd
