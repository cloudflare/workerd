// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Embedder-side support for carrying Emscripten-style WebAssembly instances through a V8 context
// snapshot.
//
// V8's snapshot serializer cannot serialize WebAssembly instances: the compiled code, linear
// memory and dispatch tables live outside the V8 heap, and `SnapshotCreator::CreateBlob()`
// aborts if any exported Wasm function is reachable from the context. This module works around
// that entirely from the embedder side:
//
//  - At snapshot-build time, `WasmSnapshotRecorder::wrap()` hides a live `WebAssembly.Instance`
//    behind a set of *stub* objects (instance, exports, memory, table, globals, tags). JS only
//    ever sees the stubs. Each exported function, and each function read out of a table, becomes
//    a small native trampoline that forwards to the live instance, so application code (including
//    Emscripten glue, which caches table entries and wraps exports) runs unmodified. Trampolines
//    and stubs may be passed back in as imports of further instances or stored into tables.
//  - `WasmSnapshotRecorder::freeze()` copies out linear memory, exported global values and
//    recorded table writes, then drops every reference to the live instance so that the
//    snapshot only contains the stubs plus an opaque payload attached via
//    `v8::SerializeInternalFieldsCallback`.
//  - After `v8::Context::FromSnapshot()`, `WasmSnapshotRestorer::hydrate()` re-instantiates each
//    module from its wire bytes (via an embedder-provided resolver), copies the captured state
//    back in, and re-binds the stubs. From then on the trampolines forward to the new instance.
//
// What the stubs cannot hide: typed-array views created over `memory.buffer` before the snapshot
// still point at a detached placeholder afterwards. Emscripten's `updateMemoryViews()` (or an
// equivalent) must be called once after `hydrate()`. Everything not visible through the JS API
// (non-exported mutable globals, funcrefs to non-exported functions, dropped passive segments,
// suspended stacks) is not preserved; a fresh instantiation supplies those.
//
// The Wasm `start` function, active data segments and element segments are re-applied during
// hydration and then overwritten by the captured state. Emscripten does not use `start`
// (constructors run from JS via `__wasm_call_ctors`), so this is side-effect free for its output.
//
// Both the isolate used with `v8::SnapshotCreator` and any isolate that deserializes the blob must
// list `getWasmSnapshotExternalReferences()` in `v8::Isolate::CreateParams::external_references`.

#include <v8.h>

#include <kj/common.h>
#include <kj/function.h>
#include <kj/string.h>
#include <kj/vector.h>

namespace workerd::jsg {

struct WrappedInstance;
struct WrappedInstanceTable;

// Native callback addresses that V8 needs to resolve when serializing or deserializing the stub
// objects. Not null-terminated; append a 0 when building `CreateParams::external_references`.
kj::ArrayPtr<const intptr_t> getWasmSnapshotExternalReferences();

// Maps a stub (memory, table, global, tag) or trampoline handed out by a recorder or restorer in
// the current isolate to the real WebAssembly object or exported function behind it. Other values
// are returned unchanged. Returns empty with a JS exception pending if the instance is not live.
// This is for APIs that only accept real Wasm objects, such as `WebAssembly.promising()`; callers
// must not keep the result anywhere that ends up in a snapshot.
v8::MaybeLocal<v8::Value> resolveWasmSnapshotValue(
    v8::Local<v8::Context> context, v8::Local<v8::Value> value);

// Snapshot-build side. One recorder serves one `v8::SnapshotCreator` isolate; it must be destroyed
// before the isolate is disposed. At most one recorder or restorer may exist per isolate.
class WasmSnapshotRecorder final {
 public:
  explicit WasmSnapshotRecorder(v8::Isolate* isolate);
  ~WasmSnapshotRecorder() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(WasmSnapshotRecorder);

  // Makes the `WebAssembly` namespace available on `context`'s global object. V8 does not expose
  // it in contexts created inside a SnapshotCreator isolate; this relies on the workerd V8 patch
  // that makes `v8::Isolate::InstallConditionalFeatures()` do so. `wrap()` and `instantiate()`
  // call this implicitly; call it directly if JS code needs `WebAssembly` before that.
  void exposeWebAssembly(v8::Local<v8::Context> context);

  // Hides `instance` (already instantiated from `module` with `imports`) behind stubs and returns
  // the instance stub object to hand to JS in place of the real one. `moduleId` is an opaque key
  // that the restorer's resolver maps back to a `WebAssembly.Module`; it is stored in the
  // snapshot instead of the wire bytes. `imports` is stored in the snapshot as-is and reused for
  // re-instantiation, so everything reachable from it must be serializable: plain JS values and
  // functions, or stubs handed out by this recorder (memories, tables, globals, tags and exported
  // functions of other wrapped instances), which are resolved to the real objects at
  // instantiation time. `Proxy` entries at the module level are passed through unmodified.
  v8::Local<v8::Object> wrap(v8::Local<v8::Context> context,
      kj::StringPtr moduleId,
      v8::Local<v8::WasmModuleObject> module,
      v8::Local<v8::Object> instance,
      v8::Local<v8::Object> imports);

  // Convenience for `new WebAssembly.Instance(module, imports)` followed by `wrap()`.
  v8::Local<v8::Object> instantiate(v8::Local<v8::Context> context,
      kj::StringPtr moduleId,
      v8::Local<v8::WasmModuleObject> module,
      v8::Local<v8::Object> imports);

  // Captures the state of every wrapped instance and detaches the stubs from the live objects.
  // Must be called before `SnapshotCreator::CreateBlob()`; after it, `memory.buffer` on every
  // stub yields a zero-length placeholder until the snapshot is hydrated, and calling exports
  // throws. The caller is responsible for refreshing any typed-array views JS holds over the old
  // buffer and for ensuring no other references to the live instance remain before creating the
  // blob: V8 links a memory's ArrayBuffer back to its WebAssembly.Memory, so a single surviving
  // view keeps the whole live instance reachable and makes `CreateBlob()` fail. This also
  // releases every v8::Global the recorder holds, as `CreateBlob()` requires that none exist.
  void freeze(v8::Local<v8::Context> context);

  // Callback to pass to `SnapshotCreator::SetDefaultContext()` / `AddContext()`.
  v8::SerializeInternalFieldsCallback serializeCallback();

 private:
  v8::Isolate* isolate;
  kj::Own<WrappedInstanceTable> instanceTable;
  kj::Vector<kj::Own<WrappedInstance>> instances;

  // Object templates for the stubs; created lazily on first `wrap()`.
  v8::Global<v8::ObjectTemplate> instanceTemplate;
  v8::Global<v8::ObjectTemplate> exportsTemplate;
  v8::Global<v8::ObjectTemplate> memoryTemplate;
  v8::Global<v8::ObjectTemplate> tableTemplate;
  v8::Global<v8::ObjectTemplate> globalTemplate;
  v8::Global<v8::ObjectTemplate> tagTemplate;
  void ensureTemplates();

  static v8::StartupData serialize(v8::Local<v8::Object> holder, int index, void* data);
};

// Restore side. One restorer serves one isolate; it owns the state behind every stub deserialized
// through its callback and must outlive the contexts that contain them. At most one recorder or
// restorer may exist per isolate.
class WasmSnapshotRestorer final {
 public:
  explicit WasmSnapshotRestorer(v8::Isolate* isolate);
  ~WasmSnapshotRestorer() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(WasmSnapshotRestorer);

  // Callback to pass to `v8::Context::FromSnapshot()`. V8 invokes it while deserialization is
  // still in progress (no GC, no JS, no compilation), so it only records the payload; the real
  // work happens in `hydrate()`.
  v8::DeserializeInternalFieldsCallback deserializeCallback();

  // Maps the `moduleId` given to `WasmSnapshotRecorder::wrap()` back to a compiled module.
  using ModuleResolver =
      kj::FunctionParam<v8::Local<v8::WasmModuleObject>(v8::Local<v8::Context>, kj::StringPtr)>;

  // Called for each re-created instance right after instantiation and before its captured
  // memory, globals and tables are restored, with the module id and the real (unwrapped)
  // instance. Emscripten output needs its `__wasm_call_ctors` run here: constructors initialize
  // state outside linear memory (e.g. entries in non-exported tables) that a fresh instance lacks.
  using AfterInstantiate = kj::FunctionParam<void(
      v8::Local<v8::Context>, kj::StringPtr moduleId, v8::Local<v8::Object> instance)>;

  // Re-instantiates every stub deserialized so far and restores its captured state. Must be
  // called after `Context::FromSnapshot()` returns and before any JS in the context touches a
  // stub. Returns the number of instances hydrated.
  size_t hydrate(v8::Local<v8::Context> context,
      ModuleResolver resolver,
      AfterInstantiate afterInstantiate = [](auto, auto, auto) {});

  // Instantiates `module` after hydration, with the stubs and trampolines among `imports` (the
  // restored instances' memory, tables and functions) replaced by the real objects they stand
  // for, as `new WebAssembly.Instance` would see them. The result is an ordinary instance: code
  // loaded after the snapshot (Emscripten shared libraries) is not recorded. Throws a KJ
  // exception carrying the JS message if instantiation fails.
  v8::Local<v8::Object> instantiate(v8::Local<v8::Context> context,
      v8::Local<v8::WasmModuleObject> module,
      v8::Local<v8::Object> imports);

 private:
  v8::Isolate* isolate;
  kj::Own<WrappedInstanceTable> instanceTable;
  kj::Vector<kj::Own<WrappedInstance>> instances;
  size_t hydrated = 0;

  static void deserialize(
      v8::Local<v8::Object> holder, int index, v8::StartupData payload, void* data);
};

}  // namespace workerd::jsg
