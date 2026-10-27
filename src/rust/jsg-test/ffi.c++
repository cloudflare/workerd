// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/features.h>
#include <workerd/jsg/ser.h>
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
#include <kj/map.h>

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
  // Lets tests optimize functions on demand to reach fast callbacks.
  static ::workerd::jsg::V8System v8System({"--allow-natives-syntax"});
  return v8System;
}

TestHarness::TestHarness(
    ::workerd::jsg::V8StackScope&, ::rust::Slice<const uint8_t> flags, FastApi fastApi)
    : isolate(kj::heap<TestIsolate>(getV8System(), kj::heap<::workerd::jsg::IsolateObserver>())),
      locker(isolate->getIsolate()),
      isolateScope(isolate->getIsolate()),
      realm(::workerd::rust::jsg::realm_create(isolate->getIsolate(), flags)) {
  if (fastApi == FastApi::Enabled) {
    isolate->setFastApiEnabledForTest();
  }

  KJ_REQUIRE(
      flags.size() % sizeof(capnp::word) == 0, "flags size must be a multiple of the word size");
  auto words = kj::heapArray<capnp::word>(flags.size() / sizeof(capnp::word));
  words.asBytes().copyFrom(kj::arrayPtr(flags.data(), flags.size()));
  // A canonical message is a single segment with no segment table.
  kj::ArrayPtr<const capnp::word> segments[1] = {words};
  capnp::SegmentArrayMessageReader reader(segments);
  isolate->runInLockScope([&](TestIsolate::Lock& lock) {
    configureJsgIsolate(lock, reader.getRoot<CompatibilityFlags>());
  });
}

kj::Own<TestHarness> create_test_harness(::rust::Slice<const uint8_t> flags, FastApi fast_api) {
  return ::workerd::jsg::runInV8Stack([flags, fast_api](::workerd::jsg::V8StackScope& stackScope) {
    return kj::heap<TestHarness>(stackScope, flags, fast_api);
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
  v8::TryCatch catcher(v8Isolate);

  v8::MaybeLocal<v8::Value> value = script->Run(ctx);
  bool success = !value.IsEmpty();
  if (!success && catcher.HasCaught()) {
    value = catcher.Exception();
  }

  return EvalResult{
    .success = success,
    .value = ::workerd::rust::jsg::maybe_local_to_ffi(value),
  };
}
}  // namespace

EvalResult EvalContext::eval(::rust::Str code, kj::Maybe<::rust::Str> maybeResourceName) const {
  // Entered again so the code can be evaluated while another isolate is entered.
  v8::Isolate::Scope isolateScope(v8Isolate);
  v8::Local<v8::Context> ctx = v8Context.Get(v8Isolate);
  v8::Context::Scope contextScope(ctx);

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

namespace {
// Keeps the callbacks of the builtin modules Rust registers.
struct RecordingModuleRegistry final: public ::workerd::rust::jsg::ModuleRegistry {
  ~RecordingModuleRegistry() noexcept override = default;

  void addBuiltinModule(::rust::Str specifier,
      ::workerd::rust::jsg::ModuleCallback callback,
      ::workerd::rust::jsg::ModuleType) override {
    modules.insert(kj::str(specifier), callback);
  }

  kj::HashMap<kj::String, ::workerd::rust::jsg::ModuleCallback> modules;
};
}  // namespace

EvalResult EvalContext::call_builtin_module(
    ::rust::Fn<void(::workerd::rust::jsg::ModuleRegistry&)> registerModules,
    ::rust::Str specifier) const {
  RecordingModuleRegistry registry;
  registerModules(registry);
  auto name = kj::str(specifier);
  auto& callback = KJ_REQUIRE_NONNULL(registry.modules.find(name), "no such builtin module", name);

  v8::Isolate::Scope isolateScope(v8Isolate);
  v8::Context::Scope contextScope(v8Context.Get(v8Isolate));
  v8::TryCatch catcher(v8Isolate);
  auto value = ::workerd::rust::jsg::maybe_local_from_ffi<v8::Value>(callback(v8Isolate));
  bool success = !value.IsEmpty();
  if (!success && catcher.HasCaught()) {
    value = catcher.Exception();
  }
  return EvalResult{
    .success = success,
    .value = ::workerd::rust::jsg::maybe_local_to_ffi(value),
  };
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

void TestHarness::drop_realm_then_gc(size_t data, ::rust::Fn<void(size_t)> callback) const {
  isolate->runInLockScope([&](TestIsolate::Lock& lock) {
    realm = kj::none;
    callback(data);
    request_gc(lock.v8Isolate, GcType::Full);
  });
}

void terminate_and_deliver(Isolate* isolate) {
  isolate->TerminateExecution();
  // The loop's stack check delivers the termination: V8 throws the termination exception and
  // the script returns empty. No TryCatch here, so the exception stays pending for the caller.
  auto context = isolate->GetCurrentContext();
  auto source = v8::String::NewFromUtf8Literal(isolate, "(() => { for (;;) {} })()");
  auto script = v8::Script::Compile(context, source).ToLocalChecked();
  KJ_ASSERT(script->Run(context).IsEmpty());
  KJ_ASSERT(isolate->IsExecutionTerminating());
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

void run_microtasks(Isolate* isolate) {
  isolate->PerformMicrotaskCheckpoint();
}

void run_unlocked(Isolate* isolate, size_t data, ::rust::Fn<void(size_t)> callback) {
  v8::Unlocker unlocker(isolate);
  callback(data);
}

size_t used_global_handles_size(Isolate* isolate) {
  v8::HeapStatistics stats;
  isolate->GetHeapStatistics(&stats);
  return stats.used_global_handles_size();
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

bool cpp_wrapped_function_data_is_rust_wrappable(Isolate* isolate) {
  auto& js = ::workerd::jsg::Lock::from(isolate);
  auto context = isolate->GetCurrentContext();
  bool found = false;
  auto function = js.wrapReturningFunction(context,
      [&found](::workerd::jsg::Lock& js,
          const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
    auto& args = const_cast<v8::FunctionCallbackInfo<v8::Value>&>(info);
    found = ::workerd::rust::jsg::fci_get_data_wrappable(&args) != kj::none;
    return js.v8Undefined();
  });
  ::workerd::jsg::check(function->Call(context, v8::Undefined(isolate), 0, nullptr));
  return found;
}

::workerd::rust::jsg::MaybeLocal empty_maybe_local() {
  return ::workerd::rust::jsg::maybe_local_to_ffi(v8::MaybeLocal<v8::Value>());
}

bool cpp_sees_empty_maybe_local(::workerd::rust::jsg::MaybeLocal value) {
  return ::workerd::rust::jsg::maybe_local_from_ffi<v8::Value>(kj::mv(value)).IsEmpty();
}

::rust::Vec<uint8_t> cpp_serialize(Isolate* isolate,
    ::workerd::rust::jsg::Local value,
    const ::workerd::rust::jsg::SerializeOptions& options) {
  auto& js = ::workerd::jsg::Lock::from(isolate);
  ::workerd::jsg::Serializer::Options cppOptions{
    .omitHeader = options.omit_header,
    .treatClassInstancesAsPlainObjects = options.class_instances_as_plain_objects,
  };
  if (options.has_version) cppOptions.version = options.version;
  ::workerd::jsg::Serializer serializer(js, kj::mv(cppOptions));
  serializer.write(
      js, ::workerd::jsg::JsValue(::workerd::rust::jsg::local_from_ffi<v8::Value>(kj::mv(value))));
  ::rust::Vec<uint8_t> result;
  for (auto byte: serializer.release().data) {
    result.push_back(byte);
  }
  return result;
}

::workerd::rust::jsg::Local cpp_deserialize(Isolate* isolate,
    ::rust::Slice<const uint8_t> data,
    const ::workerd::rust::jsg::DeserializeOptions& options) {
  auto& js = ::workerd::jsg::Lock::from(isolate);
  ::workerd::jsg::Deserializer::Options cppOptions{
    .readHeader = options.read_header,
    .preserveStackInErrors = options.preserve_stack_in_errors,
  };
  if (options.has_version) cppOptions.version = options.version;
  ::workerd::jsg::Deserializer deserializer(
      js, kj::arrayPtr(data.data(), data.size()), kj::none, kj::none, kj::mv(cppOptions));
  v8::Local<v8::Value> result = deserializer.readValue(js);
  return ::workerd::rust::jsg::to_ffi(kj::mv(result));
}

bool isolates_share_group(Isolate* a, Isolate* b) {
  return a->GetGroup() == b->GetGroup();
}

void hold_across_gc(Isolate* isolate,
    ::rust::Fn<::workerd::rust::jsg::MaybeLocal(Isolate*)> create,
    ::rust::Fn<void(Isolate*, ::workerd::rust::jsg::Local)> check) {
  v8::Global<v8::Value> held;
  {
    v8::HandleScope scope(isolate);
    v8::Local<v8::Value> value;
    KJ_ASSERT(
        ::workerd::rust::jsg::maybe_local_from_ffi<v8::Value>(create(isolate)).ToLocal(&value));
    held.Reset(isolate, value);
  }
  request_gc(isolate, GcType::Full);
  {
    v8::HandleScope scope(isolate);
    check(isolate, ::workerd::rust::jsg::to_ffi(held.Get(isolate)));
  }
  held.Reset();
  request_gc(isolate, GcType::Full);
}

}  // namespace rust::jsg_test

}  // namespace workerd
