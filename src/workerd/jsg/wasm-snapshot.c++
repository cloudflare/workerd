// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wasm-snapshot.h"

#include <workerd/jsg/embedder-data-tag.h>

#include <kj/debug.h>
#include <kj/map.h>
#include <kj/mutex.h>
#include <kj/one-of.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace workerd::jsg {

namespace {

// Tag for the aligned pointer stored in the instance stub's internal field.
constexpr v8::EmbedderDataTypeTag kWrappedInstanceTag =
    embedderDataTag(EmbedderDataTag::WASM_WRAPPED_INSTANCE);

// Internal field layouts.
//
// Instance stub (3 fields): [0] WrappedInstance* (aligned pointer; the only field that goes
// through the serialize/deserialize callbacks), [1] imports object, [2] live cache: a JS Map from
// real exported function to export name while the instance is live, undefined otherwise.
//
// The pointer in [0] is what identifies a instance stub (see wrappedOfInstanceStub()): V8 hands back an
// aligned pointer only to a reader using the tag it was stored with, and kWrappedInstanceTag is
// used for nothing else, so a non-null result proves the object is ours.
constexpr int kInstanceStateField = 0;
constexpr int kInstanceImportsField = 1;
constexpr int kInstanceLiveCacheField = 2;
constexpr int kInstanceFieldCount = 3;
// Export stub (3 fields): [0] owning instance stub, [1] export name, [2] for tables: JS Map from
// table index to the trampoline handed out for that index. An export stub is identified by its
// brand, a private property holding its ExportStubKind (see describeExportStub()).
constexpr int kExportStubOwnerField = 0;
constexpr int kExportStubNameField = 1;
constexpr int kExportStubCacheField = 2;
constexpr int kExportStubFieldCount = 3;

// What an export stub stands in for: the instance's `exports` object itself, or one of its
// non-function exports. (Function exports are trampolines, not stubs.)
enum class ExportStubKind : int { EXPORTS = 0, MEMORY = 1, TABLE = 2, GLOBAL = 3, TAG = 4 };

constexpr uint32_t kPayloadMagic = 0x504e5357;  // "WSNP"
constexpr uint32_t kPayloadVersion = 4;

// Trampolines are API functions. V8 serializes their FunctionTemplateInfo (including the `data`
// value) into the isolate part of the snapshot, so `data` must not reference any object of the
// context. It is therefore a Smi packing the wrapped instance's id and an index into its list of
// function sources; the WrappedInstance itself is found through a per-isolate registry. The same
// Smi is stored on the function object under a private symbol so that a trampoline handed back to
// us (table.set(), imports) can be identified.
//
// Layout (29 bits, fits a Smi on all platforms): [28..20] instance id, [19..0] index.
constexpr int kIndexBits = 20;
constexpr int kInstanceIdBits = 9;
constexpr uint32_t kMaxIndex = 1u << kIndexBits;
constexpr uint32_t kMaxInstances = 1u << kInstanceIdBits;
constexpr uint32_t kMaxPacked = 1u << (kIndexBits + kInstanceIdBits);

constexpr size_t kWasmPageSize = 65536;

struct TrampolineRef {
  uint32_t instanceId;
  uint32_t index;  // into WrappedInstance::functions

  uint32_t pack() const {
    return (instanceId << kIndexBits) | index;
  }
  static TrampolineRef unpack(uint32_t packed) {
    return {
      .instanceId = (packed >> kIndexBits) & (kMaxInstances - 1),
      .index = packed & (kMaxIndex - 1),
    };
  }
};

// A table entry for which a trampoline has been handed out.
struct TableRef {
  uint32_t table;  // index into WrappedInstance::tableExports
  uint32_t index;  // element index
};

// Where a trampoline finds the real function it forwards to: an export of the real instance, by
// name, or an entry of one of its tables. Table entries get trampolines of their own because
// Emscripten reads function pointers out of its indirect function table, and those functions
// need not be exported.
struct ExportName {
  kj::String name;
};
using FunctionSource = kj::OneOf<ExportName, TableRef>;

// A `table.set()` performed through a stub before `freeze()`: the trampoline stored, or none for
// null.
struct TableWrite {
  uint32_t table;  // index into tableExports
  uint32_t index;
  kj::Maybe<TrampolineRef> value;
};

struct MemoryCapture {
  kj::String name;
  uint32_t pages;
  // Contents with trailing zero bytes trimmed; the rest of the memory is zero.
  kj::Array<kj::byte> bytes;
};

struct GlobalCapture {
  kj::String name;
  // The global's value as JS sees it: a Number for i32/f32/f64, a BigInt for i64.
  kj::OneOf<double, int64_t> value;
};

}  // namespace

// Everything the recorder or restorer knows about one WebAssembly.Instance it has hidden behind
// stubs: its identity, the export names its trampolines index, the state captured at freeze()
// for hydrate() to replay, and, while a real instance is bound, the handles to it and the stubs.
struct WrappedInstance {
  // Stable across snapshot and restore; part of the trampoline data encoding.
  uint32_t id = 0;
  kj::String moduleId;
  // The functions trampolines have been handed out for, indexed by the index in trampoline data.
  kj::Vector<FunctionSource> functions;
  kj::Vector<kj::String> tableExports;
  // Length of each table in tableExports at freeze() time, so that growth is replayed.
  kj::Vector<uint32_t> tableLengths;
  kj::Vector<MemoryCapture> memories;
  kj::Vector<GlobalCapture> globals;
  kj::Vector<TableWrite> tableWrites;

  // Whether a real instance is currently bound. False between `freeze()` and `hydrate()`.
  bool live = false;
  // Whether table writes and table entries are being recorded for a snapshot: true in a
  // recorder. In a restorer the stubs only forward, so that code loaded after the snapshot can
  // store anything in a table, and nothing accumulates.
  bool recording = false;
  // In a restorer: the real function behind each ExportName entry of `functions`, resolved on
  // the trampoline's first call, so that later calls skip the export lookup. Exports never change,
  // unlike table entries, which stay dynamic. Empty while recording (a recorder unbinds its
  // instances at freeze(), when no v8::Global may exist).
  kj::Vector<v8::Global<v8::Function>> cachedExports;
  // While a snapshot blob is being created, V8 requires that no v8::Global handles exist, so all
  // of these are empty between `WasmSnapshotRecorder::freeze()` and
  // `WasmSnapshotRestorer::hydrate()`.
  v8::Global<v8::Object> instanceStub;
  v8::Global<v8::Object> exportsStub;
  v8::Global<v8::Object> realInstance;
  v8::Global<v8::Object> realExports;

  void unbind() {
    instanceStub.Reset();
    exportsStub.Reset();
    realInstance.Reset();
    realExports.Reset();
    live = false;
  }
};

// The wrapped instances of one recorder or restorer, indexed by id. Ids are kInstanceIdBits wide,
// so a dense table is small, and the recorder assigns them consecutively anyway.
struct WrappedInstanceTable {
  WrappedInstance* byId[kMaxInstances] = {};

  WrappedInstance* find(uint32_t id) const {
    return id < kMaxInstances ? byId[id] : nullptr;
  }
  void insert(WrappedInstance& wrapped) {
    KJ_REQUIRE(
        wrapped.id < kMaxInstances, "WebAssembly snapshot wrapped id out of range", wrapped.id);
    KJ_REQUIRE(
        byId[wrapped.id] == nullptr, "duplicate WebAssembly snapshot wrapped id", wrapped.id);
    byId[wrapped.id] = &wrapped;
  }
};

namespace {

// ---------------------------------------------------------------------------------------------
// Per-isolate registry of wrapped instances, used by trampolines to find theirs from the Smi in
// their data.
//
// Trampolines are the hot path of a restored isolate (every call from JavaScript into Wasm goes
// through one), so each thread remembers the table it last found; the registry's lock is taken
// again only when the thread's isolate changes or a table is registered or unregistered anywhere
// in the process, which bumps `generation`. (An isolate's data slots and its contexts' embedder
// data slots are all taken, or the table would live there.)

struct InstanceTableRegistry {
  kj::MutexGuarded<kj::HashMap<v8::Isolate*, WrappedInstanceTable*>> tables;
  std::atomic<uint64_t> generation{0};
};

InstanceTableRegistry& instanceTables() {
  static InstanceTableRegistry registry;
  return registry;
}

void registerInstanceTable(v8::Isolate* isolate, WrappedInstanceTable& table) {
  auto& registry = instanceTables();
  auto lock = registry.tables.lockExclusive();
  KJ_REQUIRE(lock->find(isolate) == kj::none,
      "only one WasmSnapshotRecorder or WasmSnapshotRestorer may exist per isolate");
  lock->insert(isolate, &table);
  registry.generation.fetch_add(1, std::memory_order_release);
}

void unregisterInstanceTable(v8::Isolate* isolate) {
  auto& registry = instanceTables();
  auto lock = registry.tables.lockExclusive();
  lock->erase(isolate);
  registry.generation.fetch_add(1, std::memory_order_release);
}

WrappedInstanceTable* findInstanceTable(v8::Isolate* isolate) {
  struct Cached {
    v8::Isolate* isolate = nullptr;
    WrappedInstanceTable* table = nullptr;
    uint64_t generation = 0;
  };
  static thread_local Cached cached;
  auto& registry = instanceTables();
  auto generation = registry.generation.load(std::memory_order_acquire);
  if (cached.isolate == isolate && cached.generation == generation) return cached.table;
  auto lock = registry.tables.lockShared();
  cached = {
    .isolate = isolate,
    .table = lock->find(isolate).map([](auto& t) { return t; }).orDefault(nullptr),
    .generation = generation,
  };
  return cached.table;
}

WrappedInstance* findInstance(v8::Isolate* isolate, uint32_t id) {
  auto table = findInstanceTable(isolate);
  return table == nullptr ? nullptr : table->find(id);
}

// ---------------------------------------------------------------------------------------------
// Small V8 helpers

v8::Local<v8::String> v8Str(v8::Isolate* isolate, kj::StringPtr s) {
  return v8::String::NewFromUtf8(isolate, s.begin(), v8::NewStringType::kNormal, s.size())
      .ToLocalChecked();
}

kj::String toKj(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value utf8(isolate, value);
  return kj::str(kj::StringPtr(*utf8, utf8.length()));
}

v8::Local<v8::Private> trampolineDataSymbol(v8::Isolate* isolate) {
  return v8::Private::ForApi(isolate, v8Str(isolate, "workerd.wasmSnapshot.trampoline"));
}

// The brand of an export stub: a private property holding its ExportStubKind as a Smi. Private symbols
// and properties are ordinary heap values, so the brand survives the snapshot.
v8::Local<v8::Private> exportStubBrandSymbol(v8::Isolate* isolate) {
  return v8::Private::ForApi(isolate, v8Str(isolate, "workerd.wasmSnapshot.exportStub"));
}

// Wraps a region of C++ that calls into JS and converts any pending JS exception into a KJ
// exception carrying the JS message.
class JsCall {
 public:
  explicit JsCall(v8::Isolate* isolate): isolate(isolate), tryCatch(isolate) {}

  template <typename T>
  v8::Local<T> check(v8::MaybeLocal<T> maybe, kj::StringPtr what) {
    v8::Local<T> result;
    if (!maybe.ToLocal(&result)) fail(what);
    return result;
  }

  template <typename T>
  T check(v8::Maybe<T> maybe, kj::StringPtr what) {
    T result;
    if (!maybe.To(&result)) fail(what);
    return result;
  }

  [[noreturn]] void fail(kj::StringPtr what) {
    kj::String message = tryCatch.HasCaught() ? toKj(isolate, tryCatch.Exception())
                                              : kj::str("(no JavaScript exception pending)");
    KJ_FAIL_REQUIRE("JavaScript exception in WebAssembly snapshot handling", what, message);
  }

  v8::Local<v8::Value> get(
      v8::Local<v8::Context> context, v8::Local<v8::Object> object, kj::StringPtr name) {
    return check(object->Get(context, v8Str(isolate, name)), name);
  }

  v8::Local<v8::Object> getObject(
      v8::Local<v8::Context> context, v8::Local<v8::Object> object, kj::StringPtr name) {
    auto value = get(context, object, name);
    KJ_REQUIRE(value->IsObject(), "expected an object", name);
    return value.As<v8::Object>();
  }

  v8::Local<v8::Value> call(v8::Local<v8::Context> context,
      v8::Local<v8::Object> receiver,
      kj::StringPtr method,
      kj::ArrayPtr<v8::Local<v8::Value>> args) {
    auto fn = get(context, receiver, method);
    KJ_REQUIRE(fn->IsFunction(), "expected a function", method);
    return check(fn.As<v8::Function>()->Call(context, receiver, args.size(), args.begin()), method);
  }

  v8::Local<v8::Object> webAssembly(v8::Local<v8::Context> context) {
    return getObject(context, context->Global(), "WebAssembly");
  }

 private:
  v8::Isolate* isolate;
  v8::TryCatch tryCatch;
};

void throwTypeError(v8::Isolate* isolate, kj::StringPtr message) {
  isolate->ThrowException(v8::Exception::TypeError(v8Str(isolate, message)));
}

// Property of the global object holding an array of every instance stub created in the context.
// JS may drop an instance after taking its exports (Emscripten does this for the helper modules it
// builds for addFunction()), but the stub must still reach the snapshot: table entries and imports
// refer to it by instance id.
v8::Local<v8::Private> instanceListSymbol(v8::Isolate* isolate) {
  return v8::Private::ForApi(isolate, v8Str(isolate, "workerd.wasmSnapshot.instances"));
}

void anchorInstanceStub(
    JsCall& js, v8::Local<v8::Context> context, v8::Local<v8::Object> instanceStub) {
  auto isolate = v8::Isolate::GetCurrent();
  auto global = context->Global();
  auto symbol = instanceListSymbol(isolate);
  auto existing = js.check(global->GetPrivate(context, symbol), "instance list");
  v8::Local<v8::Array> list;
  if (existing->IsArray()) {
    list = existing.As<v8::Array>();
  } else {
    list = v8::Array::New(isolate);
    js.check(global->SetPrivate(context, symbol, list), "instance list");
  }
  js.check(list->Set(context, list->Length(), instanceStub), "instance list");
}

constexpr kj::StringPtr kNotHydrated =
    "WebAssembly instance is not available: the snapshot has not been hydrated yet"_kj;

// ---------------------------------------------------------------------------------------------
// Locating the wrapped instance behind a stub or trampoline

// The wrapped instance behind a instance stub, or nullptr if `object` is anything else. The
// field-count test only keeps the read in bounds; the identity check is the tagged read itself,
// which yields nullptr for a field holding a heap value or a pointer stored under another tag.
WrappedInstance* wrappedOfInstanceStub(v8::Local<v8::Object> object) {
  if (object->InternalFieldCount() != kInstanceFieldCount) return nullptr;
  return static_cast<WrappedInstance*>(
      object->GetAlignedPointerFromInternalField(kInstanceStateField, kWrappedInstanceTag));
}

// A stub standing in for one of a wrapped instance's non-function exports, or for its exports
// object.
struct ExportStub {
  WrappedInstance* wrapped;
  ExportStubKind kind;
  v8::Local<v8::Object> object;
};

// Describes `value` if it is an export stub, by its brand; anything else is none.
kj::Maybe<ExportStub> describeExportStub(
    v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
  if (!value->IsObject()) return kj::none;
  auto object = value.As<v8::Object>();
  auto isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Value> kind;
  if (!object->GetPrivate(context, exportStubBrandSymbol(isolate)).ToLocal(&kind) ||
      !kind->IsInt32()) {
    return kj::none;
  }
  KJ_ASSERT(object->InternalFieldCount() == kExportStubFieldCount);
  auto owner = object->GetInternalField(kExportStubOwnerField);
  KJ_ASSERT(owner->IsValue() && owner.As<v8::Value>()->IsObject());
  auto wrapped = wrappedOfInstanceStub(owner.As<v8::Value>().As<v8::Object>());
  KJ_ASSERT(wrapped != nullptr);
  return ExportStub{
    .wrapped = wrapped,
    .kind = static_cast<ExportStubKind>(kind.As<v8::Int32>()->Value()),
    .object = object,
  };
}

kj::Maybe<TrampolineRef> describeTrampoline(
    v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
  if (!value->IsFunction()) return kj::none;
  auto isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Value> data;
  if (!value.As<v8::Object>()->GetPrivate(context, trampolineDataSymbol(isolate)).ToLocal(&data)) {
    return kj::none;
  }
  if (!data->IsInt32()) return kj::none;
  return TrampolineRef::unpack(static_cast<uint32_t>(data.As<v8::Int32>()->Value()));
}

// Resolves the receiver of an export stub method, throwing a JS TypeError if it is not a stub of the
// expected kind or its wrapped instance is currently detached from a real one.
kj::Maybe<ExportStub> requireLiveExportStub(
    const v8::FunctionCallbackInfo<v8::Value>& info, ExportStubKind kind, kj::StringPtr what) {
  auto isolate = info.GetIsolate();
  KJ_IF_SOME(stub, describeExportStub(isolate->GetCurrentContext(), info.This())) {
    if (stub.kind != kind) {
      throwTypeError(isolate, kj::str("Receiver is not a snapshot WebAssembly ", what));
      return kj::none;
    }
    if (!stub.wrapped->live) {
      throwTypeError(isolate, kNotHydrated);
      return kj::none;
    }
    return stub;
  }
  throwTypeError(isolate, kj::str("Receiver is not a snapshot WebAssembly ", what));
  return kj::none;
}

// The real export object (memory/table/global/tag) behind an export stub of a live wrapped
// instance.
v8::MaybeLocal<v8::Object> realExportBehind(v8::Local<v8::Context> context, ExportStub& stub) {
  auto isolate = v8::Isolate::GetCurrent();
  auto name = stub.object->GetInternalField(kExportStubNameField).As<v8::Value>();
  v8::Local<v8::Value> real;
  if (!stub.wrapped->realExports.Get(isolate)->Get(context, name).ToLocal(&real)) return {};
  if (!real->IsObject()) {
    throwTypeError(isolate, "WebAssembly export is missing after re-instantiation");
    return {};
  }
  return real.As<v8::Object>();
}

// The real function a trampoline forwards to, or nothing (with a JS exception pending) if the
// trampoline's wrapped instance is not live or the reference is corrupt.
v8::MaybeLocal<v8::Function> realFunctionForTrampoline(
    v8::Local<v8::Context> context, TrampolineRef ref) {
  auto isolate = v8::Isolate::GetCurrent();
  auto wrapped = findInstance(isolate, ref.instanceId);
  if (wrapped == nullptr) {
    throwTypeError(isolate, "Corrupt WebAssembly snapshot trampoline");
    return {};
  }
  if (!wrapped->live) {
    throwTypeError(isolate, kNotHydrated);
    return {};
  }
  if (ref.index < wrapped->cachedExports.size()) {
    auto& cached = wrapped->cachedExports[ref.index];
    if (!cached.IsEmpty()) return cached.Get(isolate);
  }
  auto realExports = wrapped->realExports.Get(isolate);
  v8::Local<v8::Value> target;
  if (ref.index < wrapped->functions.size()) {
    KJ_SWITCH_ONEOF(wrapped->functions[ref.index]) {
      KJ_CASE_ONEOF(exportName, ExportName) {
        if (!realExports->Get(context, v8Str(isolate, exportName.name)).ToLocal(&target)) {
          return {};
        }
        if (!wrapped->recording && target->IsFunction()) {
          if (wrapped->cachedExports.size() < wrapped->functions.size()) {
            wrapped->cachedExports.resize(wrapped->functions.size());
          }
          wrapped->cachedExports[ref.index].Reset(isolate, target.As<v8::Function>());
        }
      }
      KJ_CASE_ONEOF(tableRef, TableRef) {
        v8::Local<v8::Value> table;
        if (!realExports->Get(context, v8Str(isolate, wrapped->tableExports[tableRef.table]))
                 .ToLocal(&table) ||
            !table->IsObject()) {
          break;
        }
        v8::Local<v8::Value> get;
        if (!table.As<v8::Object>()->Get(context, v8Str(isolate, "get")).ToLocal(&get) ||
            !get->IsFunction()) {
          break;
        }
        v8::Local<v8::Value> args[] = {v8::Uint32::NewFromUnsigned(isolate, tableRef.index)};
        if (!get.As<v8::Function>()->Call(context, table, 1, args).ToLocal(&target)) return {};
      }
    }
  }
  if (target.IsEmpty() || !target->IsFunction()) {
    throwTypeError(isolate, "WebAssembly function is missing after re-instantiation");
    return {};
  }
  return target.As<v8::Function>();
}

// Creates a trampoline function for `ref`.
v8::MaybeLocal<v8::Function> newTrampoline(
    v8::Local<v8::Context> context, TrampolineRef ref, v8::Local<v8::String> name, int length);

// Maps a value read from a real table or export list back to what JS is allowed to see: real
// exported functions of `wrapped` become their export trampolines, other functions become table
// entry trampolines (cached per index on the table stub), everything else passes through.
v8::MaybeLocal<v8::Value> wrapTableEntry(
    v8::Local<v8::Context> context, ExportStub& table, uint32_t index, v8::Local<v8::Value> value) {
  if (!value->IsFunction()) return value;
  auto isolate = v8::Isolate::GetCurrent();
  auto& wrapped = *table.wrapped;

  // Real exports of this instance map to their export trampolines.
  auto liveCache = wrapped.instanceStub.Get(isolate)->GetInternalField(kInstanceLiveCacheField);
  if (liveCache->IsValue() && liveCache.As<v8::Value>()->IsMap()) {
    v8::Local<v8::Value> exportName;
    if (!liveCache.As<v8::Value>().As<v8::Map>()->Get(context, value).ToLocal(&exportName)) {
      return {};
    }
    if (exportName->IsString()) {
      v8::Local<v8::Value> trampoline;
      if (!wrapped.exportsStub.Get(isolate)->Get(context, exportName).ToLocal(&trampoline))
        return {};
      return trampoline;
    }
  }

  auto cacheField = table.object->GetInternalField(kExportStubCacheField);
  KJ_REQUIRE(cacheField->IsValue() && cacheField.As<v8::Value>()->IsMap());
  auto cache = cacheField.As<v8::Value>().As<v8::Map>();
  auto key = v8::Uint32::NewFromUnsigned(isolate, index);
  v8::Local<v8::Value> cached;
  if (!cache->Get(context, key).ToLocal(&cached)) return {};
  if (cached->IsFunction()) return cached;
  // Once restored, an entry no trampoline was handed out for before the snapshot is simply the
  // real function.
  if (!wrapped.recording) return value;

  auto tableName = table.object->GetInternalField(kExportStubNameField).As<v8::Value>();
  uint32_t tableIndex = 0;
  {
    auto tableNameStr = toKj(isolate, tableName);
    while (tableIndex < wrapped.tableExports.size() &&
        wrapped.tableExports[tableIndex] != tableNameStr) {
      tableIndex++;
    }
    KJ_REQUIRE(tableIndex < wrapped.tableExports.size());
  }
  if (wrapped.functions.size() >= kMaxIndex) {
    throwTypeError(isolate, "too many distinct WebAssembly table entries observed for snapshot");
    return {};
  }
  TrampolineRef ref{
    .instanceId = wrapped.id,
    .index = static_cast<uint32_t>(wrapped.functions.size()),
  };
  wrapped.functions.add(TableRef{.table = tableIndex, .index = index});
  v8::Local<v8::Value> length;
  int lengthValue = 0;
  if (value.As<v8::Object>()->Get(context, v8Str(isolate, "length")).ToLocal(&length) &&
      length->IsInt32()) {
    lengthValue = length.As<v8::Int32>()->Value();
  }
  v8::Local<v8::Function> trampoline;
  if (!newTrampoline(context, ref, v8Str(isolate, kj::str("table[", index, "]")), lengthValue)
           .ToLocal(&trampoline)) {
    return {};
  }
  if (cache->Set(context, key, trampoline).IsEmpty()) return {};
  return trampoline;
}

// Resolves a value handed to us by JS (an import or a table element) to the real WebAssembly
// object it stands for: export stubs and trampolines of live instances become the real export or
// function; anything else is returned unchanged. Returns empty with a JS exception pending if a
// stub's wrapped instance is not live.
v8::MaybeLocal<v8::Value> unwrapValue(v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
  auto isolate = v8::Isolate::GetCurrent();
  KJ_IF_SOME(stub, describeExportStub(context, value)) {
    if (stub.kind == ExportStubKind::EXPORTS) return value;
    if (!stub.wrapped->live) {
      throwTypeError(isolate, kNotHydrated);
      return {};
    }
    v8::Local<v8::Object> real;
    if (!realExportBehind(context, stub).ToLocal(&real)) return {};
    return real;
  }
  KJ_IF_SOME(ref, describeTrampoline(context, value)) {
    v8::Local<v8::Function> real;
    if (!realFunctionForTrampoline(context, ref).ToLocal(&real)) return {};
    return real;
  }
  return value;
}

// Builds the imports object to instantiate `module` with from the imports object JS supplied:
// only the imports the module declares are copied, with stubs and trampolines replaced by the real
// objects. Restricting the copy to declared imports matters because embedders (Emscripten) keep
// mutating their imports object after instantiation; a module never sees those additions. Module
// entries that are Proxies (Emscripten's GOT) are read through, as instantiation would.
v8::MaybeLocal<v8::Object> resolveImports(v8::Local<v8::Context> context,
    v8::Local<v8::WasmModuleObject> module,
    v8::Local<v8::Object> imports) {
  auto isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Value> wasm, moduleCtor, importsFn, declared;
  if (!context->Global()->Get(context, v8Str(isolate, "WebAssembly")).ToLocal(&wasm) ||
      !wasm->IsObject() ||
      !wasm.As<v8::Object>()->Get(context, v8Str(isolate, "Module")).ToLocal(&moduleCtor) ||
      !moduleCtor->IsObject() ||
      !moduleCtor.As<v8::Object>()->Get(context, v8Str(isolate, "imports")).ToLocal(&importsFn) ||
      !importsFn->IsFunction()) {
    throwTypeError(isolate, "WebAssembly.Module.imports is not available");
    return {};
  }
  v8::Local<v8::Value> args[] = {module};
  if (!importsFn.As<v8::Function>()->Call(context, moduleCtor, 1, args).ToLocal(&declared) ||
      !declared->IsArray()) {
    return {};
  }
  auto result = v8::Object::New(isolate);
  auto list = declared.As<v8::Array>();
  for (uint32_t i = 0; i < list->Length(); i++) {
    v8::Local<v8::Value> entry, moduleName, name, moduleValue, value, resolved;
    if (!list->Get(context, i).ToLocal(&entry) || !entry->IsObject()) return {};
    auto descriptor = entry.As<v8::Object>();
    if (!descriptor->Get(context, v8Str(isolate, "module")).ToLocal(&moduleName)) return {};
    if (!descriptor->Get(context, v8Str(isolate, "name")).ToLocal(&name)) return {};
    if (!imports->Get(context, moduleName).ToLocal(&moduleValue)) return {};
    if (!moduleValue->IsObject()) {
      // Let instantiation produce the proper LinkError.
      if (result->Set(context, moduleName, moduleValue).IsNothing()) return {};
      continue;
    }
    if (!moduleValue.As<v8::Object>()->Get(context, name).ToLocal(&value)) return {};
    if (!unwrapValue(context, value).ToLocal(&resolved)) return {};

    v8::Local<v8::Value> target;
    if (!result->Get(context, moduleName).ToLocal(&target)) return {};
    if (!target->IsObject()) {
      target = v8::Object::New(isolate);
      if (result->Set(context, moduleName, target).IsNothing()) return {};
    }
    if (target.As<v8::Object>()->Set(context, name, resolved).IsNothing()) return {};
  }
  return result;
}

// ---------------------------------------------------------------------------------------------
// Native callbacks installed on the stubs. Every one of these must appear in
// getWasmSnapshotExternalReferences().
//
// A KJ exception escaping a callback would unwind through V8 frames and terminate the process,
// so each callback below is a `guarded()` wrapper around an *Impl that turns one into a
// JavaScript error. The Impls throw KJ exceptions only for internal invariants.

template <typename Body>
void guarded(const v8::FunctionCallbackInfo<v8::Value>& info, Body&& body) {
  KJ_IF_SOME(exception, kj::runCatchingExceptions(kj::fwd<Body>(body))) {
    auto isolate = info.GetIsolate();
    isolate->ThrowError(v8Str(isolate, exception.getDescription()));
  }
}

void trampoline(const v8::FunctionCallbackInfo<v8::Value>& info);

void trampolineImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto isolate = info.GetIsolate();
  auto context = isolate->GetCurrentContext();
  if (!info.Data()->IsInt32()) {
    return throwTypeError(isolate, "Corrupt WebAssembly snapshot trampoline");
  }
  if (info.IsConstructCall()) {
    return throwTypeError(isolate, "WebAssembly functions are not constructors");
  }
  auto ref = TrampolineRef::unpack(static_cast<uint32_t>(info.Data().As<v8::Int32>()->Value()));
  v8::Local<v8::Function> target;
  if (!realFunctionForTrampoline(context, ref).ToLocal(&target)) return;

  // Wasm exports take few arguments; avoid a heap allocation per call for the usual case.
  constexpr int kInlineArgs = 8;
  v8::Local<v8::Value> inlineArgs[kInlineArgs];
  kj::Vector<v8::Local<v8::Value>> heapArgs;
  v8::Local<v8::Value>* args = inlineArgs;
  if (info.Length() > kInlineArgs) {
    heapArgs.resize(info.Length());
    args = heapArgs.begin();
  }
  for (int i = 0; i < info.Length(); i++) args[i] = info[i];
  v8::Local<v8::Value> result;
  if (target->Call(context, v8::Undefined(isolate), info.Length(), args).ToLocal(&result)) {
    info.GetReturnValue().Set(result);
  }
}

v8::MaybeLocal<v8::Function> newTrampoline(
    v8::Local<v8::Context> context, TrampolineRef ref, v8::Local<v8::String> name, int length) {
  auto isolate = v8::Isolate::GetCurrent();
  auto data = v8::Integer::New(isolate, static_cast<int32_t>(ref.pack()));
  v8::Local<v8::Function> fn;
  if (!v8::Function::New(context, &trampoline, data, length, v8::ConstructorBehavior::kThrow)
           .ToLocal(&fn)) {
    return {};
  }
  fn->SetName(name);
  if (fn->SetPrivate(context, trampolineDataSymbol(isolate), data).IsNothing()) return {};
  return fn;
}

// Forwards `this.<method>(...args)` to the real export behind an export stub.
void forwardToReal(const v8::FunctionCallbackInfo<v8::Value>& info,
    ExportStubKind kind,
    kj::StringPtr what,
    kj::StringPtr method) {
  auto isolate = info.GetIsolate();
  auto context = isolate->GetCurrentContext();
  KJ_IF_SOME(stub, requireLiveExportStub(info, kind, what)) {
    v8::Local<v8::Object> real;
    if (!realExportBehind(context, stub).ToLocal(&real)) return;
    v8::Local<v8::Value> fn;
    if (!real->Get(context, v8Str(isolate, method)).ToLocal(&fn)) return;
    if (!fn->IsFunction()) return throwTypeError(isolate, "Export lacks expected method");
    kj::Vector<v8::Local<v8::Value>> args(info.Length());
    for (int i = 0; i < info.Length(); i++) args.add(info[i]);
    v8::Local<v8::Value> result;
    if (fn.As<v8::Function>()->Call(context, real, args.size(), args.begin()).ToLocal(&result)) {
      info.GetReturnValue().Set(result);
    }
  }
}

void forwardGetter(const v8::FunctionCallbackInfo<v8::Value>& info,
    ExportStubKind kind,
    kj::StringPtr what,
    kj::StringPtr property) {
  auto isolate = info.GetIsolate();
  auto context = isolate->GetCurrentContext();
  KJ_IF_SOME(stub, requireLiveExportStub(info, kind, what)) {
    v8::Local<v8::Object> real;
    if (!realExportBehind(context, stub).ToLocal(&real)) return;
    v8::Local<v8::Value> value;
    if (real->Get(context, v8Str(isolate, property)).ToLocal(&value)) {
      info.GetReturnValue().Set(value);
    }
  }
}

void memoryBufferGetterImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto isolate = info.GetIsolate();
  auto context = isolate->GetCurrentContext();
  KJ_IF_SOME(stub, describeExportStub(context, info.This())) {
    if (stub.kind != ExportStubKind::MEMORY) {
      return throwTypeError(isolate, "Receiver is not a snapshot WebAssembly memory");
    }
    if (!stub.wrapped->live) {
      // Between freeze() and hydrate(): hand out an empty placeholder so glue code that refreshes
      // its heap views sees a zero-length buffer instead of the old Wasm memory.
      return info.GetReturnValue().Set(v8::ArrayBuffer::New(isolate, 0));
    }
    v8::Local<v8::Object> real;
    if (!realExportBehind(context, stub).ToLocal(&real)) return;
    if (!real->IsWasmMemoryObject()) return throwTypeError(isolate, "Export is not a memory");
    return info.GetReturnValue().Set(real.As<v8::WasmMemoryObject>()->Buffer());
  }
  throwTypeError(isolate, "Receiver is not a snapshot WebAssembly memory");
}

void memoryGrowImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto isolate = info.GetIsolate();
  KJ_IF_SOME(stub, describeExportStub(isolate->GetCurrentContext(), info.This())) {
    if (stub.kind == ExportStubKind::MEMORY && !stub.wrapped->live) {
      // Growing by zero pages is how Emscripten's growMemory() refreshes its heap views; allow it
      // on a frozen memory so views can be pointed at the empty placeholder.
      uint32_t delta = 1;
      if (info.Length() >= 1 && info[0]->Uint32Value(isolate->GetCurrentContext()).To(&delta) &&
          delta == 0) {
        return info.GetReturnValue().Set(0);
      }
    }
  }
  forwardToReal(info, ExportStubKind::MEMORY, "memory", "grow");
}

void tableLengthGetterImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  forwardGetter(info, ExportStubKind::TABLE, "table", "length");
}

void tableGrowImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  forwardToReal(info, ExportStubKind::TABLE, "table", "grow");
}

void tableGetImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto isolate = info.GetIsolate();
  auto context = isolate->GetCurrentContext();
  KJ_IF_SOME(stub, requireLiveExportStub(info, ExportStubKind::TABLE, "table")) {
    v8::Local<v8::Object> real;
    if (!realExportBehind(context, stub).ToLocal(&real)) return;
    uint32_t index;
    if (info.Length() < 1 || !info[0]->Uint32Value(context).To(&index)) {
      return throwTypeError(isolate, "WebAssembly.Table.get(): invalid index");
    }
    v8::Local<v8::Value> fn;
    if (!real->Get(context, v8Str(isolate, "get")).ToLocal(&fn) || !fn->IsFunction()) return;
    v8::Local<v8::Value> args[] = {v8::Uint32::NewFromUnsigned(isolate, index)};
    v8::Local<v8::Value> result, wrapped;
    if (!fn.As<v8::Function>()->Call(context, real, 1, args).ToLocal(&result)) return;
    if (wrapTableEntry(context, stub, index, result).ToLocal(&wrapped)) {
      info.GetReturnValue().Set(wrapped);
    }
  }
}

void tableSetImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto isolate = info.GetIsolate();
  auto context = isolate->GetCurrentContext();
  KJ_IF_SOME(stub, requireLiveExportStub(info, ExportStubKind::TABLE, "table")) {
    auto& wrapped = *stub.wrapped;
    v8::Local<v8::Object> real;
    if (!realExportBehind(context, stub).ToLocal(&real)) return;

    uint32_t index;
    if (info.Length() < 1 || !info[0]->Uint32Value(context).To(&index)) {
      return throwTypeError(isolate, "WebAssembly.Table.set(): invalid index");
    }
    v8::Local<v8::Value> value = info.Length() >= 2 ? info[1] : v8::Null(isolate).As<v8::Value>();

    // Only writes that can be replayed after re-instantiation are accepted while recording:
    // null, or a function that came out of a snapshot stub (an export trampoline or a table entry
    // trampoline of any wrapped instance). Once restored, anything goes, with stubs and
    // trampolines replaced by what they stand for.
    kj::Maybe<TrampolineRef> record;
    v8::Local<v8::Value> realValue;
    if (value->IsNull()) {
      realValue = value;
    } else if (!wrapped.recording) {
      if (!unwrapValue(context, value).ToLocal(&realValue)) return;
    } else {
      KJ_IF_SOME(ref, describeTrampoline(context, value)) {
        v8::Local<v8::Function> realFn;
        if (!realFunctionForTrampoline(context, ref).ToLocal(&realFn)) return;
        realValue = realFn;
        record = ref;
      } else {
        return throwTypeError(isolate,
            "WebAssembly.Table.set(): only null or functions obtained from a snapshot "
            "WebAssembly instance can be stored in a table that is part of a snapshot");
      }
    }

    auto tableName =
        toKj(isolate, stub.object->GetInternalField(kExportStubNameField).As<v8::Value>());
    uint32_t tableIndex = 0;
    while (
        tableIndex < wrapped.tableExports.size() && wrapped.tableExports[tableIndex] != tableName) {
      tableIndex++;
    }
    KJ_REQUIRE(tableIndex < wrapped.tableExports.size());

    v8::Local<v8::Value> fn;
    if (!real->Get(context, v8Str(isolate, "set")).ToLocal(&fn) || !fn->IsFunction()) return;
    v8::Local<v8::Value> args[] = {v8::Uint32::NewFromUnsigned(isolate, index), realValue};
    v8::Local<v8::Value> result;
    if (!fn.As<v8::Function>()->Call(context, real, 2, args).ToLocal(&result)) return;

    // The trampoline previously handed out for this index (if any) now forwards to the new entry
    // by construction; drop it from the cache anyway so a fresh get() sees export identity.
    auto cacheField = stub.object->GetInternalField(kExportStubCacheField);
    if (cacheField->IsValue() && cacheField.As<v8::Value>()->IsMap()) {
      (void)cacheField.As<v8::Value>().As<v8::Map>()->Delete(
          context, v8::Uint32::NewFromUnsigned(isolate, index));
    }
    if (!wrapped.recording) return;

    TableWrite write{.table = tableIndex, .index = index, .value = record};
    // Later writes to the same index supersede earlier ones.
    for (auto& existing: wrapped.tableWrites) {
      if (existing.table == write.table && existing.index == write.index) {
        existing = write;
        return;
      }
    }
    wrapped.tableWrites.add(write);
  }
}

void globalValueGetterImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  forwardGetter(info, ExportStubKind::GLOBAL, "global", "value");
}

void globalValueSetterImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto isolate = info.GetIsolate();
  auto context = isolate->GetCurrentContext();
  KJ_IF_SOME(stub, requireLiveExportStub(info, ExportStubKind::GLOBAL, "global")) {
    v8::Local<v8::Object> real;
    if (!realExportBehind(context, stub).ToLocal(&real)) return;
    v8::Local<v8::Value> value =
        info.Length() >= 1 ? info[0] : v8::Undefined(isolate).As<v8::Value>();
    (void)real->Set(context, v8Str(isolate, "value"), value);
  }
}

void globalValueOfImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  globalValueGetterImpl(info);
}

void trampoline(const v8::FunctionCallbackInfo<v8::Value>& info) {
  guarded(info, [&]() { trampolineImpl(info); });
}
void memoryBufferGetter(const v8::FunctionCallbackInfo<v8::Value>& info) {
  guarded(info, [&]() { memoryBufferGetterImpl(info); });
}
void memoryGrow(const v8::FunctionCallbackInfo<v8::Value>& info) {
  guarded(info, [&]() { memoryGrowImpl(info); });
}
void tableLengthGetter(const v8::FunctionCallbackInfo<v8::Value>& info) {
  guarded(info, [&]() { tableLengthGetterImpl(info); });
}
void tableGrow(const v8::FunctionCallbackInfo<v8::Value>& info) {
  guarded(info, [&]() { tableGrowImpl(info); });
}
void tableGet(const v8::FunctionCallbackInfo<v8::Value>& info) {
  guarded(info, [&]() { tableGetImpl(info); });
}
void tableSet(const v8::FunctionCallbackInfo<v8::Value>& info) {
  guarded(info, [&]() { tableSetImpl(info); });
}
void globalValueGetter(const v8::FunctionCallbackInfo<v8::Value>& info) {
  guarded(info, [&]() { globalValueGetterImpl(info); });
}
void globalValueSetter(const v8::FunctionCallbackInfo<v8::Value>& info) {
  guarded(info, [&]() { globalValueSetterImpl(info); });
}
void globalValueOf(const v8::FunctionCallbackInfo<v8::Value>& info) {
  guarded(info, [&]() { globalValueOfImpl(info); });
}

const intptr_t kExternalReferences[] = {
  reinterpret_cast<intptr_t>(&trampoline),
  reinterpret_cast<intptr_t>(&memoryBufferGetter),
  reinterpret_cast<intptr_t>(&memoryGrow),
  reinterpret_cast<intptr_t>(&tableLengthGetter),
  reinterpret_cast<intptr_t>(&tableGrow),
  reinterpret_cast<intptr_t>(&tableGet),
  reinterpret_cast<intptr_t>(&tableSet),
  reinterpret_cast<intptr_t>(&globalValueGetter),
  reinterpret_cast<intptr_t>(&globalValueSetter),
  reinterpret_cast<intptr_t>(&globalValueOf),
};

// ---------------------------------------------------------------------------------------------
// Payload encoding

class PayloadWriter {
 public:
  void u8(uint8_t v) {
    buf.add(static_cast<char>(v));
  }
  void u32(uint32_t v) {
    raw(&v, sizeof(v));
  }
  void u64(uint64_t v) {
    raw(&v, sizeof(v));
  }
  void f64(double v) {
    raw(&v, sizeof(v));
  }
  void i64(int64_t v) {
    raw(&v, sizeof(v));
  }
  void bytes(kj::ArrayPtr<const kj::byte> b) {
    u64(b.size());
    raw(b.begin(), b.size());
  }
  void string(kj::StringPtr s) {
    u32(s.size());
    raw(s.begin(), s.size());
  }
  void strings(const kj::Vector<kj::String>& list) {
    u32(static_cast<uint32_t>(list.size()));
    for (auto& s: list) string(s);
  }
  kj::Array<char> finish() {
    return buf.releaseAsArray();
  }

 private:
  kj::Vector<char> buf;

  void raw(const void* p, size_t n) {
    buf.addAll(kj::arrayPtr(reinterpret_cast<const char*>(p), n));
  }
};

class PayloadReader {
 public:
  explicit PayloadReader(kj::ArrayPtr<const char> data): data(data) {}

  uint8_t u8() {
    uint8_t v;
    raw(&v, sizeof(v));
    return v;
  }
  uint32_t u32() {
    uint32_t v;
    raw(&v, sizeof(v));
    return v;
  }
  uint64_t u64() {
    uint64_t v;
    raw(&v, sizeof(v));
    return v;
  }
  double f64() {
    double v;
    raw(&v, sizeof(v));
    return v;
  }
  int64_t i64() {
    int64_t v;
    raw(&v, sizeof(v));
    return v;
  }
  kj::Array<kj::byte> bytes() {
    auto n = u64();
    KJ_REQUIRE(n <= data.size() - pos, "truncated WebAssembly snapshot payload");
    auto result = kj::heapArray<kj::byte>(n);
    raw(result.begin(), n);
    return result;
  }
  kj::String string() {
    auto n = u32();
    KJ_REQUIRE(n <= data.size() - pos, "truncated WebAssembly snapshot payload");
    auto result = kj::heapString(n);
    raw(result.begin(), n);
    return result;
  }
  void strings(kj::Vector<kj::String>& out) {
    for (uint32_t n = u32(); n > 0; n--) out.add(string());
  }
  bool atEnd() {
    return pos == data.size();
  }

 private:
  kj::ArrayPtr<const char> data;
  size_t pos = 0;

  void raw(void* p, size_t n) {
    KJ_REQUIRE(n <= data.size() - pos, "truncated WebAssembly snapshot payload");
    memcpy(p, data.begin() + pos, n);
    pos += n;
  }
};

kj::Array<char> encodeInstance(const WrappedInstance& wrapped) {
  PayloadWriter w;
  w.u32(kPayloadMagic);
  w.u32(kPayloadVersion);
  w.u32(wrapped.id);
  w.string(wrapped.moduleId);
  w.strings(wrapped.tableExports);
  for (uint32_t i = 0; i < wrapped.tableExports.size(); i++) {
    w.u32(i < wrapped.tableLengths.size() ? wrapped.tableLengths[i] : 0);
  }
  w.u32(static_cast<uint32_t>(wrapped.functions.size()));
  for (auto& source: wrapped.functions) {
    KJ_SWITCH_ONEOF(source) {
      KJ_CASE_ONEOF(exportName, ExportName) {
        w.u8(0);
        w.string(exportName.name);
      }
      KJ_CASE_ONEOF(tableRef, TableRef) {
        w.u8(1);
        w.u32(tableRef.table);
        w.u32(tableRef.index);
      }
    }
  }
  w.u32(static_cast<uint32_t>(wrapped.memories.size()));
  for (auto& m: wrapped.memories) {
    w.string(m.name);
    w.u32(m.pages);
    w.bytes(m.bytes);
  }
  w.u32(static_cast<uint32_t>(wrapped.globals.size()));
  for (auto& g: wrapped.globals) {
    w.string(g.name);
    KJ_SWITCH_ONEOF(g.value) {
      KJ_CASE_ONEOF(number, double) {
        w.u8(0);
        w.f64(number);
      }
      KJ_CASE_ONEOF(bigint, int64_t) {
        w.u8(1);
        w.i64(bigint);
      }
    }
  }
  w.u32(static_cast<uint32_t>(wrapped.tableWrites.size()));
  for (auto& t: wrapped.tableWrites) {
    w.u32(t.table);
    w.u32(t.index);
    // A trampoline reference travels in its packed form; ~0 stands for null.
    w.u32(t.value.map([](const TrampolineRef& ref) { return ref.pack(); }).orDefault(~0u));
  }
  return w.finish();
}

kj::Own<WrappedInstance> decodeInstance(kj::ArrayPtr<const char> data) {
  PayloadReader r(data);
  KJ_REQUIRE(r.u32() == kPayloadMagic, "not a WebAssembly snapshot payload");
  KJ_REQUIRE(r.u32() == kPayloadVersion, "unsupported WebAssembly snapshot payload version");
  auto wrapped = kj::heap<WrappedInstance>();
  wrapped->id = r.u32();
  wrapped->moduleId = r.string();
  r.strings(wrapped->tableExports);
  for (uint32_t i = 0; i < wrapped->tableExports.size(); i++) wrapped->tableLengths.add(r.u32());
  for (uint32_t n = r.u32(); n > 0; n--) {
    switch (r.u8()) {
      case 0:
        wrapped->functions.add(ExportName{r.string()});
        break;
      case 1: {
        TableRef ref;
        ref.table = r.u32();
        ref.index = r.u32();
        KJ_REQUIRE(
            ref.table < wrapped->tableExports.size(), "corrupt WebAssembly snapshot payload");
        wrapped->functions.add(ref);
        break;
      }
      default:
        KJ_FAIL_REQUIRE("corrupt WebAssembly snapshot payload");
    }
  }
  KJ_REQUIRE(wrapped->functions.size() <= kMaxIndex, "corrupt WebAssembly snapshot payload");
  for (uint32_t n = r.u32(); n > 0; n--) {
    MemoryCapture m;
    m.name = r.string();
    m.pages = r.u32();
    m.bytes = r.bytes();
    wrapped->memories.add(kj::mv(m));
  }
  for (uint32_t n = r.u32(); n > 0; n--) {
    GlobalCapture g;
    g.name = r.string();
    switch (r.u8()) {
      case 0:
        g.value = r.f64();
        break;
      case 1:
        g.value = r.i64();
        break;
      default:
        KJ_FAIL_REQUIRE("corrupt WebAssembly snapshot payload");
    }
    wrapped->globals.add(kj::mv(g));
  }
  for (uint32_t n = r.u32(); n > 0; n--) {
    TableWrite t;
    t.table = r.u32();
    t.index = r.u32();
    auto packed = r.u32();
    if (packed != ~0u) {
      KJ_REQUIRE(packed < kMaxPacked, "corrupt WebAssembly snapshot payload");
      t.value = TrampolineRef::unpack(packed);
    }
    KJ_REQUIRE(t.table < wrapped->tableExports.size(), "corrupt WebAssembly snapshot payload");
    wrapped->tableWrites.add(t);
  }
  KJ_REQUIRE(r.atEnd(), "trailing bytes in WebAssembly snapshot payload");
  return wrapped;
}

// ---------------------------------------------------------------------------------------------
// Export enumeration

enum class ExportKind { FUNCTION, TABLE, MEMORY, GLOBAL, TAG, OTHER };

struct ExportDescriptor {
  kj::String name;
  ExportKind kind;
};

kj::Vector<ExportDescriptor> describeExports(
    JsCall& js, v8::Local<v8::Context> context, v8::Local<v8::WasmModuleObject> module) {
  auto isolate = v8::Isolate::GetCurrent();
  auto moduleCtor = js.getObject(context, js.webAssembly(context), "Module");
  v8::Local<v8::Value> args[] = {module};
  auto list = js.call(context, moduleCtor, "exports", args);
  KJ_REQUIRE(list->IsArray(), "WebAssembly.Module.exports() did not return an array");
  auto array = list.As<v8::Array>();
  kj::Vector<ExportDescriptor> result(array->Length());
  for (uint32_t i = 0; i < array->Length(); i++) {
    auto entry = js.check(array->Get(context, i), "exports[i]");
    KJ_REQUIRE(entry->IsObject());
    auto object = entry.As<v8::Object>();
    auto name = toKj(isolate, js.get(context, object, "name"));
    auto kindStr = toKj(isolate, js.get(context, object, "kind"));
    ExportKind kind = kindStr == "function" ? ExportKind::FUNCTION
        : kindStr == "table"                ? ExportKind::TABLE
        : kindStr == "memory"               ? ExportKind::MEMORY
        : kindStr == "global"               ? ExportKind::GLOBAL
        : kindStr == "tag"                  ? ExportKind::TAG
                                            : ExportKind::OTHER;
    result.add(ExportDescriptor{.name = kj::mv(name), .kind = kind});
  }
  return result;
}

// Returns the original WebAssembly.<name> constructor. Embedders may have replaced it with a
// wrapper that routes back into a recorder; the original is still reachable through the shared
// prototype's `constructor`.
v8::Local<v8::Function> originalWasmConstructor(
    JsCall& js, v8::Local<v8::Context> context, kj::StringPtr name) {
  auto ctor = js.get(context, js.webAssembly(context), name);
  KJ_REQUIRE(ctor->IsFunction(), "WebAssembly constructor is not a function", name);
  auto proto = js.get(context, ctor.As<v8::Object>(), "prototype");
  if (proto->IsObject()) {
    auto original = js.get(context, proto.As<v8::Object>(), "constructor");
    if (original->IsFunction()) ctor = original;
  }
  return ctor.As<v8::Function>();
}

v8::Local<v8::Object> newWebAssemblyInstance(JsCall& js,
    v8::Local<v8::Context> context,
    v8::Local<v8::WasmModuleObject> module,
    v8::Local<v8::Object> imports) {
  auto resolved = js.check(resolveImports(context, module, imports), "resolving snapshot imports");
  auto ctor = originalWasmConstructor(js, context, "Instance");
  v8::Local<v8::Value> args[] = {module, resolved};
  return js.check(ctor->NewInstance(context, 2, args), "new WebAssembly.Instance");
}

v8::Local<v8::Object> newInstanceFromTemplate(
    v8::Local<v8::Context> context, v8::Global<v8::ObjectTemplate>& tmpl) {
  return tmpl.Get(v8::Isolate::GetCurrent())->NewInstance(context).ToLocalChecked();
}

void defineReadOnly(v8::Local<v8::Context> context,
    v8::Local<v8::Object> object,
    v8::Local<v8::Name> name,
    v8::Local<v8::Value> value) {
  auto attrs = static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete);
  bool ok;
  KJ_REQUIRE(object->DefineOwnProperty(context, name, value, attrs).To(&ok) && ok);
}

// Binds a wrapped instance to a real one: sets the real handles and builds the live cache (real function
// export -> export name) on the instance stub.
void bindInstance(JsCall& js,
    v8::Local<v8::Context> context,
    WrappedInstance& wrapped,
    v8::Local<v8::Object> instance,
    v8::Local<v8::Object> realExports) {
  auto isolate = v8::Isolate::GetCurrent();
  auto liveCache = v8::Map::New(isolate);
  for (auto& source: wrapped.functions) {
    KJ_IF_SOME(exportName, source.tryGet<ExportName>()) {
      auto nameStr = v8Str(isolate, exportName.name);
      auto fn = js.check(realExports->Get(context, nameStr), exportName.name);
      KJ_REQUIRE(fn->IsFunction(), "export is not a function after instantiation", exportName.name);
      js.check(liveCache->Set(context, fn, nameStr), "live cache");
    }
  }
  wrapped.instanceStub.Get(isolate)->SetInternalField(kInstanceLiveCacheField, liveCache);
  wrapped.realInstance.Reset(isolate, instance);
  wrapped.realExports.Reset(isolate, realExports);
  wrapped.live = true;
}

void unbindInstance(WrappedInstance& wrapped) {
  auto isolate = v8::Isolate::GetCurrent();
  if (!wrapped.instanceStub.IsEmpty()) {
    wrapped.instanceStub.Get(isolate)->SetInternalField(
        kInstanceLiveCacheField, v8::Undefined(isolate));
  }
  wrapped.unbind();
}

}  // namespace

// =============================================================================================
// WasmSnapshotRecorder

WasmSnapshotRecorder::WasmSnapshotRecorder(v8::Isolate* isolate)
    : isolate(isolate),
      instanceTable(kj::heap<WrappedInstanceTable>()) {
  registerInstanceTable(isolate, *instanceTable);
}

WasmSnapshotRecorder::~WasmSnapshotRecorder() noexcept(false) {
  unregisterInstanceTable(isolate);
}

void WasmSnapshotRecorder::ensureTemplates() {
  if (!instanceTemplate.IsEmpty()) return;
  v8::HandleScope scope(isolate);

  auto method = [&](v8::FunctionCallback cb) { return v8::FunctionTemplate::New(isolate, cb); };

  auto instance = v8::ObjectTemplate::New(isolate);
  instance->SetInternalFieldCount(kInstanceFieldCount);
  instanceTemplate.Reset(isolate, instance);

  auto exports = v8::ObjectTemplate::New(isolate);
  exports->SetInternalFieldCount(kExportStubFieldCount);
  exportsTemplate.Reset(isolate, exports);

  auto memory = v8::ObjectTemplate::New(isolate);
  memory->SetInternalFieldCount(kExportStubFieldCount);
  memory->SetAccessorProperty(v8Str(isolate, "buffer"), method(&memoryBufferGetter));
  memory->Set(v8Str(isolate, "grow"), method(&memoryGrow));
  memoryTemplate.Reset(isolate, memory);

  auto table = v8::ObjectTemplate::New(isolate);
  table->SetInternalFieldCount(kExportStubFieldCount);
  table->SetAccessorProperty(v8Str(isolate, "length"), method(&tableLengthGetter));
  table->Set(v8Str(isolate, "get"), method(&tableGet));
  table->Set(v8Str(isolate, "set"), method(&tableSet));
  table->Set(v8Str(isolate, "grow"), method(&tableGrow));
  tableTemplate.Reset(isolate, table);

  auto global = v8::ObjectTemplate::New(isolate);
  global->SetInternalFieldCount(kExportStubFieldCount);
  global->SetAccessorProperty(
      v8Str(isolate, "value"), method(&globalValueGetter), method(&globalValueSetter));
  global->Set(v8Str(isolate, "valueOf"), method(&globalValueOf));
  globalTemplate.Reset(isolate, global);

  // Tags have no JS-observable API beyond identity; they only exist so they can be passed back
  // as imports to other instances.
  auto tag = v8::ObjectTemplate::New(isolate);
  tag->SetInternalFieldCount(kExportStubFieldCount);
  tagTemplate.Reset(isolate, tag);
}

void WasmSnapshotRecorder::exposeWebAssembly(v8::Local<v8::Context> context) {
  KJ_REQUIRE(v8::Isolate::GetCurrent() == isolate);
  v8::HandleScope scope(isolate);
  auto isExposed = [&]() {
    v8::Local<v8::Value> ns;
    return context->Global()->Get(context, v8Str(isolate, "WebAssembly")).ToLocal(&ns) &&
        ns->IsObject();
  };
  if (isExposed()) return;
  isolate->InstallConditionalFeatures(context);
  KJ_REQUIRE(isExposed(),
      "WebAssembly is not available in this context. Contexts created inside a "
      "v8::SnapshotCreator only expose it when V8 carries the workerd patch that makes "
      "Isolate::InstallConditionalFeatures() install it (patches/v8/*-Expose-WebAssembly-in-"
      "SnapshotCreator-contexts-on-re.patch).");
}

v8::Local<v8::Object> WasmSnapshotRecorder::instantiate(v8::Local<v8::Context> context,
    kj::StringPtr moduleId,
    v8::Local<v8::WasmModuleObject> module,
    v8::Local<v8::Object> imports) {
  exposeWebAssembly(context);
  v8::EscapableHandleScope scope(isolate);
  JsCall js(isolate);
  auto instance = newWebAssemblyInstance(js, context, module, imports);
  return scope.Escape(wrap(context, moduleId, module, instance, imports));
}

v8::Local<v8::Object> WasmSnapshotRecorder::wrap(v8::Local<v8::Context> context,
    kj::StringPtr moduleId,
    v8::Local<v8::WasmModuleObject> module,
    v8::Local<v8::Object> instance,
    v8::Local<v8::Object> imports) {
  KJ_REQUIRE(v8::Isolate::GetCurrent() == isolate);
  exposeWebAssembly(context);
  ensureTemplates();
  v8::EscapableHandleScope scope(isolate);
  JsCall js(isolate);

  auto realExports = js.getObject(context, instance, "exports");
  auto descriptors = describeExports(js, context, module);

  KJ_REQUIRE(instances.size() < kMaxInstances, "too many WebAssembly instances in one snapshot");
  auto wrapped = kj::heap<WrappedInstance>();
  wrapped->id = static_cast<uint32_t>(instances.size());
  wrapped->moduleId = kj::str(moduleId);
  wrapped->recording = true;

  auto instanceStub = newInstanceFromTemplate(context, instanceTemplate);
  instanceStub->SetAlignedPointerInInternalField(
      kInstanceStateField, wrapped.get(), kWrappedInstanceTag);
  instanceStub->SetInternalField(kInstanceImportsField, imports);
  instanceStub->SetInternalField(kInstanceLiveCacheField, v8::Undefined(isolate));

  auto newExportStub = [&](v8::Global<v8::ObjectTemplate>& tmpl, ExportStubKind kind,
                           v8::Local<v8::String> name) {
    auto stub = newInstanceFromTemplate(context, tmpl);
    js.check(stub->SetPrivate(context, exportStubBrandSymbol(isolate),
                 v8::Integer::New(isolate, static_cast<int>(kind))),
        "branding export stub");
    stub->SetInternalField(kExportStubOwnerField, instanceStub);
    stub->SetInternalField(kExportStubNameField, name);
    stub->SetInternalField(kExportStubCacheField,
        kind == ExportStubKind::TABLE ? v8::Map::New(isolate).As<v8::Value>()
                                      : v8::Undefined(isolate).As<v8::Value>());
    return stub;
  };

  auto exportsStub = newExportStub(exportsTemplate, ExportStubKind::EXPORTS, v8Str(isolate, ""));

  for (auto& desc: descriptors) {
    auto name = v8Str(isolate, desc.name);
    v8::Local<v8::Value> stub;
    switch (desc.kind) {
      case ExportKind::FUNCTION: {
        KJ_REQUIRE(wrapped->functions.size() < kMaxIndex,
            "too many function exports for a snapshot WebAssembly instance");
        TrampolineRef ref{
          .instanceId = wrapped->id,
          .index = static_cast<uint32_t>(wrapped->functions.size()),
        };
        wrapped->functions.add(ExportName{kj::str(desc.name)});
        auto real = js.get(context, realExports, desc.name);
        KJ_REQUIRE(real->IsFunction(), "export is not a function", desc.name);
        auto length = js.get(context, real.As<v8::Object>(), "length");
        stub = js.check(newTrampoline(context, ref, name,
                            length->IsInt32() ? length.As<v8::Int32>()->Value() : 0),
            "creating trampoline");
        break;
      }
      case ExportKind::MEMORY:
        stub = newExportStub(memoryTemplate, ExportStubKind::MEMORY, name);
        break;
      case ExportKind::TABLE:
        wrapped->tableExports.add(kj::str(desc.name));
        stub = newExportStub(tableTemplate, ExportStubKind::TABLE, name);
        break;
      case ExportKind::GLOBAL:
        stub = newExportStub(globalTemplate, ExportStubKind::GLOBAL, name);
        break;
      case ExportKind::TAG:
        stub = newExportStub(tagTemplate, ExportStubKind::TAG, name);
        break;
      case ExportKind::OTHER:
        KJ_FAIL_REQUIRE("WebAssembly export kind is not supported in snapshots", desc.name);
    }
    defineReadOnly(context, exportsStub, name, stub);
  }

  defineReadOnly(context, instanceStub, v8Str(isolate, "exports"), exportsStub);

  anchorInstanceStub(js, context, instanceStub);
  wrapped->instanceStub.Reset(isolate, instanceStub);
  wrapped->exportsStub.Reset(isolate, exportsStub);
  bindInstance(js, context, *wrapped, instance, realExports);
  instanceTable->insert(*wrapped);
  instances.add(kj::mv(wrapped));
  return scope.Escape(instanceStub);
}

void WasmSnapshotRecorder::freeze(v8::Local<v8::Context> context) {
  KJ_REQUIRE(v8::Isolate::GetCurrent() == isolate);
  v8::HandleScope scope(isolate);
  JsCall js(isolate);

  for (auto& wrapped: instances) {
    if (!wrapped->live) continue;
    auto realExports = wrapped->realExports.Get(isolate);
    auto exportsStub = wrapped->exportsStub.Get(isolate);
    wrapped->memories.clear();
    wrapped->globals.clear();
    wrapped->tableLengths.clear();
    for (auto& tableName: wrapped->tableExports) {
      auto table = js.getObject(context, realExports, tableName);
      auto length = js.get(context, table, "length");
      wrapped->tableLengths.add(length->IsUint32() ? length.As<v8::Uint32>()->Value() : 0);
    }

    auto names = js.check(exportsStub->GetOwnPropertyNames(context), "GetOwnPropertyNames");
    for (uint32_t i = 0; i < names->Length(); i++) {
      auto nameValue = js.check(names->Get(context, i), "names[i]");
      auto name = toKj(isolate, nameValue);
      auto exported = js.check(exportsStub->Get(context, nameValue), name);
      auto stub = describeExportStub(context, exported);
      if (stub == kj::none) continue;  // function trampoline
      auto real = js.check(realExports->Get(context, nameValue), name);
      switch (KJ_ASSERT_NONNULL(stub).kind) {
        case ExportStubKind::MEMORY: {
          KJ_REQUIRE(real->IsWasmMemoryObject());
          auto buffer = real.As<v8::WasmMemoryObject>()->Buffer();
          auto contents =
              kj::arrayPtr(static_cast<const kj::byte*>(buffer->Data()), buffer->ByteLength());
          size_t end = contents.size();
          while (end > 0 && contents[end - 1] == 0) end--;
          wrapped->memories.add(MemoryCapture{
            .name = kj::mv(name),
            .pages = static_cast<uint32_t>(contents.size() / kWasmPageSize),
            .bytes = kj::heapArray<kj::byte>(contents.first(end)),
          });
          break;
        }
        case ExportStubKind::GLOBAL: {
          KJ_REQUIRE(real->IsObject());
          auto value = js.get(context, real.As<v8::Object>(), "value");
          if (value->IsBigInt()) {
            wrapped->globals.add(
                GlobalCapture{.name = kj::mv(name), .value = value.As<v8::BigInt>()->Int64Value()});
          } else if (value->IsNumber()) {
            wrapped->globals.add(
                GlobalCapture{.name = kj::mv(name), .value = value.As<v8::Number>()->Value()});
          } else {
            KJ_FAIL_REQUIRE(
                "exported WebAssembly global has a type that snapshots cannot capture", name);
          }
          break;
        }
        case ExportStubKind::TABLE:
        case ExportStubKind::TAG:
        case ExportStubKind::EXPORTS:
          break;
      }
    }

    unbindInstance(*wrapped);
  }

  // CreateBlob() refuses to run while any v8::Global exists in the isolate, so drop the templates
  // too; ensureTemplates() recreates them if wrap() is called again.
  instanceTemplate.Reset();
  exportsTemplate.Reset();
  memoryTemplate.Reset();
  tableTemplate.Reset();
  globalTemplate.Reset();
  tagTemplate.Reset();
}

v8::SerializeInternalFieldsCallback WasmSnapshotRecorder::serializeCallback() {
  return v8::SerializeInternalFieldsCallback(&WasmSnapshotRecorder::serialize, this);
}

v8::StartupData WasmSnapshotRecorder::serialize(
    v8::Local<v8::Object> holder, int index, void* data) {
  if (holder->InternalFieldCount() != kInstanceFieldCount || index != kInstanceStateField) {
    // Not one of ours; let V8 serialize the field verbatim.
    return {nullptr, 0};
  }
  auto wrapped = wrappedOfInstanceStub(holder);
  KJ_REQUIRE(wrapped != nullptr);
  KJ_REQUIRE(!wrapped->live,
      "WasmSnapshotRecorder::freeze() must be called before creating the snapshot blob");
  auto payload = encodeInstance(*wrapped);
  KJ_REQUIRE(
      payload.size() <= static_cast<size_t>(INT32_MAX), "WebAssembly snapshot payload too large");
  // V8 takes ownership of the returned buffer and releases it with delete[].
  auto copy = new char[payload.size()];
  memcpy(copy, payload.begin(), payload.size());
  return v8::StartupData{copy, static_cast<int>(payload.size())};
}

// =============================================================================================
// WasmSnapshotRestorer

WasmSnapshotRestorer::WasmSnapshotRestorer(v8::Isolate* isolate)
    : isolate(isolate),
      instanceTable(kj::heap<WrappedInstanceTable>()) {
  registerInstanceTable(isolate, *instanceTable);
}

WasmSnapshotRestorer::~WasmSnapshotRestorer() noexcept(false) {
  unregisterInstanceTable(isolate);
}

v8::DeserializeInternalFieldsCallback WasmSnapshotRestorer::deserializeCallback() {
  return v8::DeserializeInternalFieldsCallback(&WasmSnapshotRestorer::deserialize, this);
}

void WasmSnapshotRestorer::deserialize(
    v8::Local<v8::Object> holder, int index, v8::StartupData payload, void* data) {
  auto& self = *static_cast<WasmSnapshotRestorer*>(data);
  if (payload.raw_size == 0) return;
  KJ_REQUIRE(holder->InternalFieldCount() == kInstanceFieldCount && index == kInstanceStateField,
      "unexpected embedder field payload in snapshot");
  auto wrapped = decodeInstance(kj::arrayPtr(payload.data, payload.raw_size));
  self.instanceTable->insert(*wrapped);
  holder->SetAlignedPointerInInternalField(kInstanceStateField, wrapped.get(), kWrappedInstanceTag);
  wrapped->instanceStub.Reset(self.isolate, holder);
  self.instances.add(kj::mv(wrapped));
}

v8::Local<v8::Object> WasmSnapshotRestorer::instantiate(v8::Local<v8::Context> context,
    v8::Local<v8::WasmModuleObject> module,
    v8::Local<v8::Object> imports) {
  KJ_REQUIRE(v8::Isolate::GetCurrent() == isolate);
  v8::EscapableHandleScope scope(isolate);
  JsCall js(isolate);
  return scope.Escape(newWebAssemblyInstance(js, context, module, imports));
}

size_t WasmSnapshotRestorer::hydrate(
    v8::Local<v8::Context> context, ModuleResolver resolver, AfterInstantiate afterInstantiate) {
  KJ_REQUIRE(v8::Isolate::GetCurrent() == isolate);
  v8::HandleScope scope(isolate);
  JsCall js(isolate);

  // Instances are re-created in creation order so that an instance importing another wrapped
  // instance's memory, table or functions finds them live already.
  kj::Vector<WrappedInstance*> pending;
  for (auto& wrapped: instances) {
    if (!wrapped->live) pending.add(wrapped.get());
  }
  std::sort(pending.begin(), pending.end(),
      [](WrappedInstance* a, WrappedInstance* b) { return a->id < b->id; });

  for (auto wrapped: pending) {
    auto instanceStub = wrapped->instanceStub.Get(isolate);
    auto exportsStub = js.getObject(context, instanceStub, "exports");
    // Needed by the trampoline/export mapping helpers as soon as the instance is bound.
    wrapped->exportsStub.Reset(isolate, exportsStub);
    auto importsField = instanceStub->GetInternalField(kInstanceImportsField);
    KJ_REQUIRE(importsField->IsValue() && importsField.As<v8::Value>()->IsObject(),
        "snapshot instance stub lost its imports object");
    auto imports = importsField.As<v8::Value>().As<v8::Object>();

    auto module = resolver(context, wrapped->moduleId);
    KJ_REQUIRE(!module.IsEmpty(), "module resolver returned nothing", wrapped->moduleId);
    auto instance = newWebAssemblyInstance(js, context, module, imports);
    auto realExports = js.getObject(context, instance, "exports");
    afterInstantiate(context, wrapped->moduleId, instance);

    for (auto& m: wrapped->memories) {
      auto memory = js.get(context, realExports, m.name);
      KJ_REQUIRE(memory->IsWasmMemoryObject(), "snapshot memory export is not a memory", m.name);
      auto wasmMemory = memory.As<v8::WasmMemoryObject>();
      size_t currentPages = wasmMemory->Buffer()->ByteLength() / kWasmPageSize;
      if (currentPages < m.pages) {
        v8::Local<v8::Value> args[] = {
          v8::Uint32::NewFromUnsigned(isolate, static_cast<uint32_t>(m.pages - currentPages))};
        js.call(context, wasmMemory, "grow", args);
      }
      auto buffer = wasmMemory->Buffer();
      KJ_REQUIRE(
          buffer->ByteLength() >= m.bytes.size(), "restored memory is smaller than snapshot");
      memcpy(buffer->Data(), m.bytes.begin(), m.bytes.size());
      // Anything beyond the captured prefix is zero in a fresh memory already, except for bytes
      // written by active data segments; those must be cleared to match the snapshot.
      if (buffer->ByteLength() > m.bytes.size()) {
        memset(static_cast<kj::byte*>(buffer->Data()) + m.bytes.size(), 0,
            buffer->ByteLength() - m.bytes.size());
      }
      // The image is in the memory now; a second copy would outlive the isolate's memory limit.
      m.bytes = nullptr;
    }

    for (auto& g: wrapped->globals) {
      auto global = js.getObject(context, realExports, g.name);
      v8::Local<v8::Value> wanted;
      KJ_SWITCH_ONEOF(g.value) {
        KJ_CASE_ONEOF(number, double) {
          wanted = v8::Number::New(isolate, number);
        }
        KJ_CASE_ONEOF(bigint, int64_t) {
          wanted = v8::BigInt::New(isolate, bigint);
        }
      }
      auto current = js.get(context, global, "value");
      // Immutable globals cannot be assigned, but they also cannot differ from a fresh
      // instantiation, so only write when the value actually changed.
      if (!current->StrictEquals(wanted)) {
        js.check(global->Set(context, v8Str(isolate, "value"), wanted), g.name);
      }
    }

    for (uint32_t i = 0; i < wrapped->tableExports.size() && i < wrapped->tableLengths.size();
         i++) {
      auto table = js.getObject(context, realExports, wrapped->tableExports[i]);
      auto length = js.get(context, table, "length");
      uint32_t current = length->IsUint32() ? length.As<v8::Uint32>()->Value() : 0;
      if (current < wrapped->tableLengths[i]) {
        v8::Local<v8::Value> args[] = {
          v8::Uint32::NewFromUnsigned(isolate, wrapped->tableLengths[i] - current)};
        js.call(context, table, "grow", args);
      }
    }

    bindInstance(js, context, *wrapped, instance, realExports);
  }

  // Table writes may refer to functions of instances created later than the table's owner, so
  // they are replayed once every instance is live again.
  for (auto wrapped: pending) {
    auto realExports = wrapped->realExports.Get(isolate);
    for (auto& t: wrapped->tableWrites) {
      auto table = js.getObject(context, realExports, wrapped->tableExports[t.table]);
      v8::Local<v8::Value> value = v8::Null(isolate);
      KJ_IF_SOME(ref, t.value) {
        value = js.check(realFunctionForTrampoline(context, ref), "resolving table entry");
      }
      v8::Local<v8::Value> args[] = {v8::Uint32::NewFromUnsigned(isolate, t.index), value};
      js.call(context, table, "set", args);
    }
  }

  hydrated += pending.size();
  return pending.size();
}

kj::ArrayPtr<const intptr_t> getWasmSnapshotExternalReferences() {
  return kExternalReferences;
}

v8::MaybeLocal<v8::Value> resolveWasmSnapshotValue(
    v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
  return unwrapValue(context, value);
}

}  // namespace workerd::jsg
