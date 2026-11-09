// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wasm-snapshot.h"

#include <workerd/jsg/setup.h>
#include <workerd/jsg/wasm-snapshot-test-wasm.embed.h>

#include <kj/test.h>

#include <memory>

namespace workerd::jsg::test {
namespace {

V8System v8System;

// Null-terminated external references for both isolates.
kj::Array<intptr_t> externalReferences() {
  auto refs = getWasmSnapshotExternalReferences();
  auto result = kj::heapArray<intptr_t>(refs.size() + 1);
  for (size_t i = 0; i < refs.size(); i++) result[i] = refs[i];
  result[refs.size()] = 0;
  return result;
}

v8::Local<v8::String> v8Str(v8::Isolate* isolate, kj::StringPtr s) {
  return v8::String::NewFromUtf8(isolate, s.begin(), v8::NewStringType::kNormal, s.size())
      .ToLocalChecked();
}

kj::String toKj(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value utf8(isolate, value);
  return kj::str(kj::StringPtr(*utf8, utf8.length()));
}

// Evaluates `code` in `context`, failing the test with the JS error message on exception.
v8::Local<v8::Value> eval(v8::Local<v8::Context> context, kj::StringPtr code) {
  auto isolate = v8::Isolate::GetCurrent();
  v8::EscapableHandleScope scope(isolate);
  v8::TryCatch tryCatch(isolate);
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> result;
  if (!v8::Script::Compile(context, v8Str(isolate, code)).ToLocal(&script) ||
      !script->Run(context).ToLocal(&result)) {
    KJ_FAIL_REQUIRE("JS threw", code, toKj(isolate, tryCatch.Exception()));
  }
  return scope.Escape(result);
}

kj::String evalStr(v8::Local<v8::Context> context, kj::StringPtr code) {
  v8::HandleScope scope(v8::Isolate::GetCurrent());
  return toKj(v8::Isolate::GetCurrent(), eval(context, code));
}

v8::Local<v8::WasmModuleObject> compileTestModule(v8::Isolate* isolate) {
  kj::ArrayPtr<const kj::byte> bytes = WASM_SNAPSHOT_TEST_WASM;
  return v8::WasmModuleObject::Compile(
      isolate, std::span<const uint8_t>(bytes.begin(), bytes.size()))
      .ToLocalChecked();
}

// Mirrors Emscripten's updateMemoryViews(): re-derive heap views from memory.buffer.
constexpr kj::StringPtr UPDATE_MEMORY_VIEWS =
    "globalThis.HEAPU8 = new Uint8Array(instance.exports.memory.buffer);"_kj;

KJ_TEST("WebAssembly instance survives a context snapshot via stubs") {
  auto refs = externalReferences();
  std::shared_ptr<v8::ArrayBuffer::Allocator> allocator(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());

  // ---- Snapshot build ----
  // Both phases run under runInV8Stack() because the V8 build used here requires the embedder to
  // mark the stack start before touching an isolate.
  v8::StartupData blob = runInV8Stack([&](V8StackScope&) {
    v8::Isolate::CreateParams params;
    params.external_references = refs.begin();
    params.array_buffer_allocator_shared = allocator;
    v8::SnapshotCreator creator(params);
    auto isolate = creator.GetIsolate();
    // Declared after `creator` so its v8::Globals are released before the isolate is disposed.
    WasmSnapshotRecorder recorder(isolate);
    {
      v8::HandleScope handleScope(isolate);
      auto context = v8::Context::New(isolate);
      v8::Context::Scope contextScope(context);

      {
        // Inner scope: every handle to the live instance must be gone before CreateBlob().
        v8::HandleScope innerScope(isolate);
        auto imports = eval(context,
            "globalThis.logged = [];"
            "({ env: { log(x) { globalThis.logged.push(x); } } })")
                           .As<v8::Object>();
        auto stub =
            recorder.instantiate(context, "test-module", compileTestModule(isolate), imports);
        KJ_REQUIRE(context->Global()->Set(context, v8Str(isolate, "instance"), stub).FromJust());
      }

      // Application code runs unmodified against the stubs while the real instance is live.
      eval(context, R"(
        const e = instance.exports;
        e.__wasm_call_ctors();                     // mem[16] = 42
        e.inc(); e.inc();                          // mem[16] = 44, counter = 2
        e.big.value = -(2n ** 40n) - 5n;           // i64 global, set from JS as a BigInt
        e.ratio.value = 1.5;                       // f64 global
        e.set(100, 7);
        e.memory.grow(1);                          // 2 pages
        e.set(65536 + 10, 9);
        e.__indirect_function_table.set(2, e.twice);
        e.__indirect_function_table.set(3, null);
        globalThis.cachedInc = e.inc;              // trampoline captured pre-freeze
        globalThis.smallU8 = new Uint8Array(1); smallU8[0] = 7;   // on-heap typed array
        globalThis.HEAPU8 = new Uint8Array(e.memory.buffer);
        globalThis.before = [
          HEAPU8[16], e.counter.value, e.__indirect_function_table.length,
          e.__indirect_function_table.get(0) === e.get, HEAPU8.length,
        ].join();
      )");
      KJ_EXPECT(evalStr(context, "before") == "44,2,4,true,131072");

      recorder.freeze(context);
      eval(context, UPDATE_MEMORY_VIEWS);
      KJ_EXPECT(evalStr(context, "HEAPU8.length") == "0");
      KJ_EXPECT(evalStr(context, "try { instance.exports.inc(); 'ok' } catch (e) { e.message }")
                    .contains("not been hydrated"));

      // A blob needs a default context, which must differ from any context added with
      // AddContext(): V8 serializes each registered context separately and adjusts some objects
      // destructively while doing so. Context::FromSnapshot(0) restores the added one.
      creator.SetDefaultContext(v8::Context::New(isolate));
      KJ_EXPECT(creator.AddContext(context, recorder.serializeCallback()) == 0);
    }
    return creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  });
  KJ_REQUIRE(blob.data != nullptr && blob.raw_size > 0);
  KJ_DEFER(delete[] blob.data);

  // ---- Restore ----
  runInV8Stack([&](V8StackScope&) {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.external_references = refs.begin();
    params.array_buffer_allocator_shared = allocator;
    auto isolate = v8::Isolate::New(params);
    KJ_DEFER(isolate->Dispose());
    v8::Isolate::Scope isolateScope(isolate);
    WasmSnapshotRestorer restorer(isolate);
    v8::HandleScope handleScope(isolate);

    auto context =
        v8::Context::FromSnapshot(isolate, 0, restorer.deserializeCallback()).ToLocalChecked();
    v8::Context::Scope contextScope(context);

    KJ_EXPECT(evalStr(context, "typeof instance.exports.inc") == "function");
    KJ_EXPECT(evalStr(context, "try { instance.exports.inc(); 'ok' } catch (e) { e.message }")
                  .contains("not been hydrated"));

    uint resolved = 0;
    auto count = restorer.hydrate(context, [&](v8::Local<v8::Context>, kj::StringPtr moduleId) {
      KJ_EXPECT(moduleId == "test-module");
      resolved++;
      return compileTestModule(isolate);
    });
    KJ_EXPECT(count == 1);
    KJ_EXPECT(resolved == 1);
    eval(context, UPDATE_MEMORY_VIEWS);

    // Memory: ctor/inc writes, explicit stores, growth, and the data segment all restored.
    KJ_EXPECT(evalStr(context, "HEAPU8.length") == "131072");
    KJ_EXPECT(evalStr(context, "HEAPU8[16]") == "44");
    KJ_EXPECT(evalStr(context, "instance.exports.get(16)") == "44");
    KJ_EXPECT(evalStr(context, "instance.exports.get(100)") == "7");
    KJ_EXPECT(evalStr(context, "instance.exports.get(65536 + 10)") == "9");
    KJ_EXPECT(evalStr(context, "String.fromCharCode(...HEAPU8.subarray(0, 5))") == "hello");

    // Exported mutable global restored.
    KJ_EXPECT(evalStr(context, "instance.exports.counter.value") == "2");
    KJ_EXPECT(evalStr(context, "instance.exports.big.value") == "-1099511627781");
    KJ_EXPECT(evalStr(context, "instance.exports.ratio.value") == "1.5");

    // Trampolines captured before the snapshot still work and see the new instance.
    KJ_EXPECT(evalStr(context, "cachedInc()") == "45");
    KJ_EXPECT(evalStr(context, "instance.exports.counter.value") == "3");
    KJ_EXPECT(evalStr(context, "instance.exports.inc.name") == "inc");

    // Table: element segment entries and the recorded set() calls, incl. identity mapping.
    KJ_EXPECT(evalStr(context, "instance.exports.__indirect_function_table.length") == "4");
    KJ_EXPECT(evalStr(context, "instance.exports.callIndirect(0, 16)") == "45");
    KJ_EXPECT(evalStr(context, "instance.exports.callIndirect(2, 21)") == "42");
    KJ_EXPECT(evalStr(context,
                  "instance.exports.__indirect_function_table.get(1) === instance.exports.twice") ==
        "true");
    KJ_EXPECT(evalStr(context, "instance.exports.__indirect_function_table.get(3)") == "null");

    // Small typed arrays are allocated on-heap; they must round-trip too.
    KJ_EXPECT(evalStr(context, "smallU8[0] = 24; smallU8[0] + ''") == "24");

    // Imports object came out of the snapshot and is wired to the new instance.
    KJ_EXPECT(evalStr(context, "instance.exports.callLog(5); logged.join()") == "5");

    // Stubs keep working for further mutation after hydration. Growing detaches the old buffer,
    // so the stale view goes to zero length until refreshed, as with a real WebAssembly.Memory.
    KJ_EXPECT(evalStr(context, "instance.exports.memory.grow(1); HEAPU8.length") == "0");
    eval(context, UPDATE_MEMORY_VIEWS);
    KJ_EXPECT(evalStr(context, "HEAPU8.length") == "196608");
    KJ_EXPECT(evalStr(context,
                  "instance.exports.counter.value = 10; instance.exports.counter.value") == "10");
  });
}

}  // namespace
}  // namespace workerd::jsg::test
