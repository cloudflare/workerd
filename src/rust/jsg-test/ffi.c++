// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/setup.h>
#include <workerd/rust/jsg-test/lib.rs.h>
#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/jsg/ffi.h>
#include <workerd/rust/jsg/lib.rs.h>
#include <workerd/rust/jsg/v8.rs.h>

#include <cppgc/common.h>
#include <v8-cppgc.h>
#include <v8.h>

#include <capnp/message.h>
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
      realm([&] {
        // Build default (all-false) feature flags for the test realm.
        capnp::MallocMessageBuilder flagsMessage;
        flagsMessage.initRoot<CompatibilityFlags>();
        auto words = capnp::canonicalize(flagsMessage.getRoot<CompatibilityFlags>().asReader());
        return ::workerd::rust::jsg::realm_create(
            isolate->getIsolate(), words.asBytes().as<Rust>());
      }()) {
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
  KJ_ASSERT(v8::Script::Compile(ctx, source).ToLocal(&script), "Failed to compile script");
  v8::TryCatch catcher(v8Isolate);

  v8::Local<v8::Value> value;
  if (script->Run(ctx).ToLocal(&value)) {
    result.success = true;
    result.value = ::workerd::rust::jsg::to_ffi(kj::mv(value));
  } else if (catcher.HasCaught()) {
    result.success = false;
    auto exception = catcher.Exception();
    result.value = ::workerd::rust::jsg::to_ffi(kj::mv(exception));
  } else {
    result.success = false;
  }

  return result;
}

void TestHarness::run_in_context(
    size_t data, ::rust::Fn<void(size_t, Isolate*, EvalContext&)> callback) const {
  isolate->runInLockScope([&](TestIsolate::Lock& lock) {
    auto context = lock.newContext<TestContext>();
    v8::Local<v8::Context> v8Context = context.getHandle(lock.v8Isolate);
    v8::Context::Scope contextScope(v8Context);

    EvalContext evalContext(lock.v8Isolate, v8Context);
    callback(data, lock.v8Isolate, evalContext);
  });
}

void request_gc(Isolate* isolate, GcType gc_type) {
  switch (gc_type) {
    case GcType::Full:
      isolate->RequestGarbageCollectionForTesting(
          v8::Isolate::GarbageCollectionType::kFullGarbageCollection);
      if (auto* cppHeap = isolate->GetCppHeap()) {
        cppHeap->CollectGarbageForTesting(cppgc::EmbedderStackState::kNoHeapPointers);
      }
      break;
    case GcType::Minor:
      isolate->RequestGarbageCollectionForTesting(
          v8::Isolate::GarbageCollectionType::kMinorGarbageCollection);
      // V8's minor GC (scavenge) only collects V8's young generation heap.
      // CppgcShim objects live on the cppgc heap, so we must also trigger
      // cppgc's young generation collection to actually reclaim them.
      if (auto* cppHeap = isolate->GetCppHeap()) {
        cppHeap->CollectGarbageInYoungGenerationForTesting(
            cppgc::EmbedderStackState::kNoHeapPointers);
      }
      break;
  }
}

::workerd::rust::jsg::Local create_cpp_tagged_object(Isolate* isolate) {
  auto tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->SetInternalFieldCount(::workerd::jsg::Wrappable::INTERNAL_FIELD_COUNT);
  auto obj = ::workerd::jsg::check(tmpl->NewInstance(isolate->GetCurrentContext()));

  // Set the C++ wrappable tag (NOT the Rust tag) to simulate a C++ JSG object
  auto tagAddress = const_cast<uint16_t*>(&::workerd::jsg::Wrappable::WORKERD_WRAPPABLE_TAG);
  obj->SetAlignedPointerInInternalField(::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX,
      tagAddress,
      static_cast<v8::EmbedderDataTypeTag>(::workerd::jsg::Wrappable::WRAPPABLE_TAG_FIELD_INDEX));

  return ::workerd::rust::jsg::to_ffi(v8::Local<v8::Value>::Cast(obj));
}

}  // namespace rust::jsg_test

}  // namespace workerd
