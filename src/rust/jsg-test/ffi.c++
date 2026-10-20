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

TestHarness::TestHarness(::workerd::jsg::V8StackScope&, ::rust::Slice<const uint8_t> flags)
    : isolate(kj::heap<TestIsolate>(getV8System(), kj::heap<::workerd::jsg::IsolateObserver>())),
      locker(isolate->getIsolate()),
      isolateScope(isolate->getIsolate()),
      realm(::workerd::rust::jsg::realm_create(isolate->getIsolate(), flags)) {
  auto* v8Isolate = isolate->getIsolate();
  v8Isolate->SetData(::workerd::jsg::SetDataIndex::SET_DATA_RUST_REALM, &*realm);

  // Mirrors the subset of `Worker::Isolate`'s flag handling that affects how resource
  // templates are built.
  KJ_REQUIRE(
      flags.size() % sizeof(capnp::word) == 0, "flags size must be a multiple of the word size");
  auto words = kj::heapArray<capnp::word>(flags.size() / sizeof(capnp::word));
  words.asBytes().copyFrom(kj::arrayPtr(flags.data(), flags.size()));
  // A canonical message is a single segment with no segment table.
  kj::ArrayPtr<const capnp::word> segments[1] = {words};
  capnp::SegmentArrayMessageReader reader(segments);
  auto features = reader.getRoot<CompatibilityFlags>();
  auto& isolateBase = ::workerd::jsg::IsolateBase::from(v8Isolate);
  if (features.getSetToStringTag()) {
    isolateBase.enableSetToStringTag();
  }
  if (features.getShouldSetImmutablePrototype() || features.getPythonWorkers()) {
    isolateBase.enableSetImmutablePrototype();
  }
  if (features.getSpecCompliantPropertyAttributes()) {
    isolateBase.enableSpecCompliantPropertyAttributes();
  }
}

kj::Own<TestHarness> create_test_harness(::rust::Slice<const uint8_t> flags) {
  return ::workerd::jsg::runInV8Stack([flags](::workerd::jsg::V8StackScope& stackScope) {
    return kj::heap<TestHarness>(stackScope, flags);
  });
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

namespace {
EvalResult runScript(
    v8::Isolate* v8Isolate, v8::Local<v8::Context> ctx, v8::Local<v8::Script> script) {
  EvalResult result;
  result.success = false;

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
}  // namespace

EvalResult EvalContext::eval(::rust::Str code, kj::Maybe<::rust::Str> maybeResourceName) const {
  v8::Local<v8::Context> ctx = v8Context.Get(v8Isolate);

  v8::Local<v8::String> source = ::workerd::jsg::check(v8::String::NewFromUtf8(
      v8Isolate, code.data(), v8::NewStringType::kNormal, static_cast<int>(code.size())));

  v8::Local<v8::Script> script;
  KJ_IF_SOME(resourceName, maybeResourceName) {
    auto resourceNameStr = ::workerd::jsg::check(v8::String::NewFromUtf8(v8Isolate,
        resourceName.data(), v8::NewStringType::kNormal, static_cast<int>(resourceName.size())));
    v8::ScriptOrigin origin(resourceNameStr);
    v8::ScriptCompiler::Source scriptSource(source, origin);
    script = ::workerd::jsg::check(v8::ScriptCompiler::Compile(ctx, &scriptSource));
  } else {
    script = ::workerd::jsg::check(v8::Script::Compile(ctx, source));
  }

  return runScript(v8Isolate, ctx, script);
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
