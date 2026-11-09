// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "snapshot-harness.h"

#include <workerd/api/snapshot-harness-shims.embed.h>
#include <workerd/jsg/wasm-snapshot.h>
#include <workerd/jsg/wrappable.h>
#include <workerd/util/entropy.h>

#include <kj/debug.h>
#include <kj/map.h>
#include <kj/mutex.h>
#include <kj/time.h>
#include <kj/vector.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

namespace workerd::api::pyodide {

namespace {

constexpr kj::StringPtr kHarnessGlobal = "__pyodideSnapshotHarness"_kj;
constexpr kj::StringPtr kMainModuleName = "pyodide-snapshot-harness:main"_kj;
constexpr kj::StringPtr kWasmModulePrefix = "inline-wasm:"_kj;

// A WebAssembly.Module stub is a plain object branded with a private property holding its module
// id, so that only objects the harness made are taken for stubs.
constexpr kj::StringPtr kModuleStubBrand = "pyodideSnapshotHarness.moduleId"_kj;

// ---------------------------------------------------------------------------------------------
// V8 helpers

v8::Local<v8::String> v8Str(v8::Isolate* isolate, kj::StringPtr s) {
  return v8::String::NewFromUtf8(isolate, s.begin(), v8::NewStringType::kNormal, s.size())
      .ToLocalChecked();
}

kj::String toKj(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value utf8(isolate, value);
  return kj::str(kj::StringPtr(*utf8, utf8.length()));
}

void throwTypeError(v8::Isolate* isolate, kj::StringPtr message) {
  isolate->ThrowException(v8::Exception::TypeError(v8Str(isolate, message)));
}

kj::String describeException(v8::Isolate* isolate, v8::TryCatch& tryCatch) {
  if (!tryCatch.HasCaught()) return kj::str("(no exception)");
  auto context = isolate->GetCurrentContext();
  auto exception = tryCatch.Exception();
  kj::String message = toKj(isolate, exception);
  if (exception->IsObject()) {
    v8::Local<v8::Value> stack;
    if (exception.As<v8::Object>()->Get(context, v8Str(isolate, "stack")).ToLocal(&stack) &&
        stack->IsString()) {
      message = toKj(isolate, stack);
    }
  }
  auto v8Message = tryCatch.Message();
  if (!v8Message.IsEmpty()) {
    auto name = toKj(isolate, v8Message->GetScriptResourceName());
    int line = v8Message->GetLineNumber(context).FromMaybe(0);
    return kj::str(message, "\n    at ", name, ":", line);
  }
  return message;
}

// Fails with the pending JS exception if `maybe` is empty.
template <typename T>
v8::Local<T> check(
    v8::Isolate* isolate, v8::TryCatch& tryCatch, v8::MaybeLocal<T> maybe, kj::StringPtr what) {
  v8::Local<T> result;
  if (!maybe.ToLocal(&result)) {
    KJ_FAIL_REQUIRE("JavaScript exception in Pyodide snapshot harness", what,
        describeException(isolate, tryCatch));
  }
  return result;
}

// ---------------------------------------------------------------------------------------------
// Module sources

struct ModuleSource {
  enum Kind { ESM, DATA, WASM, JSON } kind;
  // For ESM and JSON, the source text; for DATA and WASM, the bytes. Points into the bundles or
  // into static shim sources; both outlive the harness.
  kj::ArrayPtr<const kj::byte> bytes;
};

// JS shims for the C++-backed builtin modules Pyodide's TypeScript imports during a baseline
// load. Only what the load path calls is implemented, with the answers a plain workerd worker
// without packages and without a memory snapshot would get.
constexpr kj::StringPtr kMetadataShim = R"JS(
class PyodideMetadataReader {
  isWorkerd() { return true; }
  isTracing() { return false; }
  getMainModule() { return 'worker.py'; }
  getNames() { return []; }
  getSizes() { return []; }
  read() { return 0; }
  hasMemorySnapshot() { return false; }
  getMemorySnapshotSize() { return 0; }
  readMemorySnapshot() { return 0; }
  disposeMemorySnapshot() {}
  shouldSnapshotToDisk() { return false; }
  getPyodideVersion() { return globalThis.__pyodideSnapshotHarness.pyodideVersion(); }
  getPackagesVersion() { return ''; }
  getPackagesLock() { return '{"packages":{}}'; }
  isCreatingBaselineSnapshot() { return false; }
  getCompatibilityFlags() {
    return JSON.parse(globalThis.__pyodideSnapshotHarness.compatibilityFlags());
  }
  static getBaselineSnapshotImports() {
    return [
      '_pyodide', '_pyodide.docstring', '_pyodide._core_docs', 'traceback', 'collections.abc',
      'asyncio', 'inspect', 'tarfile', 'importlib', 'importlib.metadata', 're', 'shutil',
      'sysconfig', 'importlib.machinery', 'pathlib', 'site', 'tempfile', 'typing', 'zipfile',
    ];
  }
  setCpuLimitNearlyExceededCallback() {}
}
export default new PyodideMetadataReader();
)JS"_kj;

constexpr kj::StringPtr kArtifactsShim = R"JS(
export default {
  hasMemorySnapshot() { return false; },
  getMemorySnapshotSize() { return 0; },
  readMemorySnapshot() { return 0; },
  disposeMemorySnapshot() {},
  isEwValidating() { return false; },
  isDynamicWorker() { return false; },
  storeMemorySnapshot() { throw new Error('not supported in the snapshot harness'); },
  isEnabled() { return false; },
};
)JS"_kj;

constexpr kj::StringPtr kPackagesShim = R"JS(
export default { getFiles() { return []; } };
)JS"_kj;

// ReadOnlyBuffer over the bundle's python_stdlib.zip data module.
constexpr kj::StringPtr kPythonStdlibShim = R"JS(
import zip from 'pyodideRuntime-internal:python_stdlib.zip';
const bytes = new Uint8Array(zip);
export default {
  getSize() { return bytes.length; },
  read(offset, buf) {
    const n = Math.min(buf.length, Math.max(0, bytes.length - offset));
    buf.set(bytes.subarray(offset, offset + n));
    return n;
  },
};
)JS"_kj;

// The harness always loads Pyodide from scratch: it is what produces the snapshots.
constexpr kj::StringPtr kRestoredShim = R"JS(
export default undefined;
)JS"_kj;

constexpr kj::StringPtr kDiskCacheShim = R"JS(
export default {
  get() { return undefined; },
  put() {},
  putSnapshot() {},
};
)JS"_kj;

constexpr kj::StringPtr kJaegerShim = R"JS(
export default {};
)JS"_kj;

// Reports go to the console, which after the snapshot is restored is the host's.
constexpr kj::StringPtr kFatalReporterShim = R"JS(
export default {
  reportFatal(message) { console.error('FATAL: ' + message); },
  reportPythonWorkersInternalError(message) { console.error('INTERNAL: ' + message); },
};
)JS"_kj;

constexpr kj::StringPtr kUnsafeEvalShim = R"JS(
export default {
  newWasmModule(bytes) { return new WebAssembly.Module(bytes); },
  eval() { throw new Error('eval is not available in the snapshot harness'); },
  newFunction() { throw new Error('new Function is not available in the snapshot harness'); },
};
)JS"_kj;

constexpr kj::StringPtr kWorkersShim = R"JS(
export function abortIsolate(message) {
  throw new Error('abortIsolate: ' + message);
}
export default { abortIsolate };
)JS"_kj;

// Mirrors API.serializeHiwireState() in Pyodide, keeping the values instead of serializing them.
constexpr kj::StringPtr kCaptureHiwire = R"JS(
(() => {
  const Module = pyodide._module;
  const API = pyodide._api;
  const expected = API.getExpectedKeys().length;
  const hiwire = [];
  for (let i = expected; ; i++) {
    let value;
    try {
      value = Module.__hiwire_get(i);
    } catch {
      break;
    }
    hiwire.push(value);
  }
  // Index 6 holds Pyodide's js_no_value marker; _jslib_init() recreates entries up to there.
  const immortalStart = 7;
  if (Module.__hiwire_immortal_get(immortalStart - 1)?.noValueMarker !== 1) {
    throw new Error('unexpected Pyodide immortal table layout');
  }
  const immortal = [];
  for (let i = immortalStart; ; i++) {
    let value;
    try {
      value = Module.__hiwire_immortal_get(i);
    } catch {
      break;
    }
    immortal.push(value);
  }
  globalThis.__pyodideSnapshotHiwire = { hiwire, immortal };
})();
)JS"_kj;

// Mirrors syncUpSnapshotLoad1/2 in Pyodide's finalizeBootstrap(). Unlike Pyodide's memory
// snapshot, the JS side is not fresh here, so the JS-only bookkeeping those do
// (runtimeKeepalivePush) is not repeated.
constexpr kj::StringPtr kRestoreHiwire = R"JS(
(() => {
  const Module = pyodide._module;
  const API = pyodide._api;
  const { hiwire, immortal } = globalThis.__pyodideSnapshotHiwire;
  const step = (name, fn) => {
    try {
      return fn();
    } catch (e) {
      throw new Error(`restoring Pyodide hiwire state failed at ${name}: ${e}`);
    }
  };
  step('_jslib_init', () => Module._jslib_init());
  if (Module.__hiwire_immortal_get(6)?.noValueMarker !== 1) {
    throw new Error('unexpected Pyodide immortal table layout after _jslib_init');
  }
  step('_init_pyodide_proxy', () => Module._init_pyodide_proxy());
  const set = (index, value) => {
    if (Module.__hiwire_set(index, value) < 0) throw new Error(`hiwire set failed at ${index}`);
  };
  const expected = API.getExpectedKeys();
  step('expected keys', () => expected.forEach((value, i) => set(i, value)));
  step('hiwire keys', () => hiwire.forEach((value, i) => set(expected.length + i, value)));
  step('immortal keys', () => immortal.forEach((value) => Module.__hiwire_immortal_add(value)));
  delete globalThis.__pyodideSnapshotHiwire;
})();
)JS"_kj;

// The module that drives the load: Pyodide up to the baseline, as python.ts defines it. A rejected
// promise carries any failure.
constexpr kj::StringPtr kMainModule = R"JS(
import { loadPyodideBaseline } from 'pyodide-internal:python';
globalThis.__pyodideLoad = (async () => {
  const Module = await loadPyodideBaseline({
    pyodide_entrypoint_helper: {},
    cloudflare_compat_flags: {},
  });
  globalThis.pyodide = Module.API.public_api;
  return 'ok';
})();
)JS"_kj;

// ---------------------------------------------------------------------------------------------
// The harness

class Harness;

kj::MutexGuarded<kj::HashMap<v8::Isolate*, Harness*>>& registry() {
  static kj::MutexGuarded<kj::HashMap<v8::Isolate*, Harness*>> map;
  return map;
}

class Harness final {
 public:
  enum class Mode { BUILD, RESTORE };

  Harness(v8::Isolate* isolate,
      kj::Maybe<PumpMessageLoop> pump,
      const PyodideSnapshotArtifacts& artifacts,
      Mode mode,
      kj::ArrayPtr<const PyodideSnapshot::InlineWasm> inlineWasm,
      bool installIsolateCallbacks)
      : isolate(isolate),
        pump(kj::mv(pump)),
        artifacts(artifacts.clone()),
        mode(mode),
        startTime(kj::systemPreciseMonotonicClock().now()) {
    addBundle(artifacts.staticBundle);
    addBundle(artifacts.runtimeBundle);
    addShim("pyodide-internal:runtime-generated/metadata", kMetadataShim);
    addShim("pyodide-internal:artifacts", kArtifactsShim);
    addShim("pyodide-internal:packages", kPackagesShim);
    addShim("pyodide-internal:python_stdlib", kPythonStdlibShim);
    addShim("pyodide-internal:restored", kRestoredShim);
    addShim("pyodide-internal:disk_cache", kDiskCacheShim);
    addShim("pyodide-internal:internalJaeger", kJaegerShim);
    addShim("pyodide-internal:fatal-reporter", kFatalReporterShim);
    addShim("internal:unsafe-eval", kUnsafeEvalShim);
    addShim("cloudflare-internal:workers", kWorkersShim);
    addShim(kMainModuleName, kMainModule);
    for (auto& w: inlineWasm) {
      inlineWasmBytes.insert(kj::str(w.id), kj::heapArray<kj::byte>(w.bytes));
      // Ids are "<prefix><n>"; modules compiled after restore continue the numbering.
      if (w.id.startsWith(kWasmModulePrefix)) {
        KJ_IF_SOME(n, w.id.slice(kWasmModulePrefix.size()).tryParseAs<uint>()) {
          nextInlineWasm = kj::max(nextInlineWasm, n + 1);
        }
      }
    }
    if (mode == Mode::BUILD) {
      recorder.emplace(isolate);
    } else {
      restorer.emplace(isolate);
    }
    registry().lockExclusive()->insert(isolate, this);
    if (installIsolateCallbacks) {
      isolate->SetHostImportModuleDynamicallyCallback(&importDynamically);
      isolate->SetPromiseRejectCallback(&onPromiseReject);
      isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
    }
  }

  ~Harness() noexcept(false) {
    registry().lockExclusive()->erase(isolate);
  }

  static Harness& from(v8::Isolate* isolate) {
    auto lock = registry().lockShared();
    return *KJ_REQUIRE_NONNULL(lock->find(isolate), "no Pyodide snapshot harness for isolate");
  }

  jsg::WasmSnapshotRecorder& getRecorder() {
    return KJ_REQUIRE_NONNULL(recorder);
  }
  jsg::WasmSnapshotRestorer& getRestorer() {
    return KJ_REQUIRE_NONNULL(restorer);
  }

  // Calls `name` on the shims' harness object with `arg`, if any, as its only argument.
  void callHarnessFunction(
      v8::Local<v8::Context> context, kj::StringPtr name, kj::Maybe<v8::Local<v8::Value>> arg) {
    v8::HandleScope scope(isolate);
    v8::TryCatch tryCatch(isolate);
    auto harness = check(isolate, tryCatch,
        context->Global()->Get(context, v8Str(isolate, kHarnessGlobal)), "harness global");
    KJ_REQUIRE(harness->IsObject());
    auto fn = check(
        isolate, tryCatch, harness.As<v8::Object>()->Get(context, v8Str(isolate, name)), name);
    KJ_REQUIRE(fn->IsFunction(), "no such harness function", name);
    v8::Local<v8::Value> args[1];
    int argc = 0;
    KJ_IF_SOME(a, arg) args[argc++] = a;
    check(isolate, tryCatch, fn.As<v8::Function>()->Call(context, harness, argc, args), name);
  }

  // Installs the native harness object and the JS shims into `context`.
  void installGlobals(v8::Local<v8::Context> context) {
    v8::HandleScope scope(isolate);
    v8::TryCatch tryCatch(isolate);
    auto tmpl = v8::ObjectTemplate::New(isolate);
    auto method = [&](kj::StringPtr name, v8::FunctionCallback cb) {
      tmpl->Set(v8Str(isolate, name), v8::FunctionTemplate::New(isolate, cb));
    };
    method("print", &print);
    method("now", &now);
    method("randomFill", &randomFill);
    method("pyodideVersion", &pyodideVersion);
    method("compatibilityFlags", &compatibilityFlags);
    method("data", &data);
    method("text", &text);
    method("wasmModule", &wasmModule);
    method("compileWasm", &compileWasm);
    method("isModule", &isModule);
    method("realModule", &realModule);
    method("instantiate", &instantiate);
    method("realFunction", &realFunction);
    auto harnessObject = check(isolate, tryCatch, tmpl->NewInstance(context), "harness object");
    KJ_REQUIRE(context->Global()
                   ->Set(context, v8Str(isolate, kHarnessGlobal), harnessObject)
                   .FromMaybe(false));

    runScript(context, "snapshot-harness-shims.js", SNAPSHOT_HARNESS_SHIMS);
  }

  v8::Local<v8::Value> runScript(
      v8::Local<v8::Context> context, kj::StringPtr name, kj::StringPtr code) {
    v8::EscapableHandleScope scope(isolate);
    v8::TryCatch tryCatch(isolate);
    v8::ScriptOrigin origin(v8Str(isolate, name));
    v8::ScriptCompiler::Source source(v8Str(isolate, code), origin);
    auto script = check(isolate, tryCatch, v8::ScriptCompiler::Compile(context, &source), name);
    return scope.Escape(check(isolate, tryCatch, script->Run(context), name));
  }

  // Compiles, links and evaluates the harness main module, then drives the event loop until the
  // load promise settles.
  void loadPyodide(v8::Local<v8::Context> context) {
    v8::HandleScope scope(isolate);
    v8::TryCatch tryCatch(isolate);
    auto module = check(isolate, tryCatch, getModule(context, kMainModuleName), "main module");
    KJ_REQUIRE(module->InstantiateModule(context, &resolveModule).FromMaybe(false),
        "linking the Pyodide module graph failed", describeException(isolate, tryCatch));
    auto evaluation = check(isolate, tryCatch, module->Evaluate(context), "evaluating main");
    KJ_REQUIRE(evaluation->IsPromise());
    drain(context, evaluation.As<v8::Promise>());
    auto load = check(isolate, tryCatch,
        context->Global()->Get(context, v8Str(isolate, "__pyodideLoad")), "__pyodideLoad");
    KJ_REQUIRE(load->IsPromise());
    auto promise = load.As<v8::Promise>();
    drain(context, promise);
    KJ_REQUIRE(promise->State() == v8::Promise::kFulfilled, "loading Pyodide failed",
        describeValue(context, promise->Result()));
  }

  // Runs microtasks, platform tasks and timers until `promise` settles.
  void drain(v8::Local<v8::Context> context, v8::Local<v8::Promise> promise) {
    auto& pumpFn = KJ_REQUIRE_NONNULL(pump, "this harness has no message loop pump");
    v8::HandleScope scope(isolate);
    for (int idle = 0;;) {
      isolate->PerformMicrotaskCheckpoint();
      while (pumpFn(isolate)) {
        isolate->PerformMicrotaskCheckpoint();
      }
      if (promise->State() != v8::Promise::kPending) return;
      auto timers = runTimers(context);
      if (timers.ran > 0) continue;
      if (timers.nextDueMs >= 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int64_t>(timers.nextDueMs) + 1));
        continue;
      }
      // Nothing scheduled in JS; background work (e.g. Wasm compilation) may still post tasks.
      KJ_REQUIRE(++idle < 500,
          "Pyodide snapshot harness event loop is stuck: promise pending with no timers, "
          "microtasks or platform tasks");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  kj::String describeValue(v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
    if (value->IsObject()) {
      v8::Local<v8::Value> stack;
      if (value.As<v8::Object>()->Get(context, v8Str(isolate, "stack")).ToLocal(&stack) &&
          stack->IsString()) {
        return toKj(isolate, stack);
      }
    }
    return toKj(isolate, value);
  }

  void callExportIfPresent(
      v8::Local<v8::Context> context, v8::Local<v8::Object> instance, kj::StringPtr name) {
    v8::HandleScope scope(isolate);
    v8::TryCatch tryCatch(isolate);
    auto exports =
        check(isolate, tryCatch, instance->Get(context, v8Str(isolate, "exports")), "exports");
    KJ_REQUIRE(exports->IsObject());
    auto fn = check(
        isolate, tryCatch, exports.As<v8::Object>()->Get(context, v8Str(isolate, name)), name);
    if (!fn->IsFunction()) return;
    check(isolate, tryCatch,
        fn.As<v8::Function>()->Call(context, v8::Undefined(isolate), 0, nullptr), name);
  }

  // Releases every v8::Global the harness holds, as required before CreateBlob().
  void releaseHandles() {
    modules.clear();
    realModules.clear();
  }

  kj::Array<PyodideSnapshot::InlineWasm> takeInlineWasm() {
    kj::Vector<PyodideSnapshot::InlineWasm> result;
    for (auto& entry: inlineWasmBytes) {
      result.add(PyodideSnapshot::InlineWasm{
        .id = kj::str(entry.key), .bytes = kj::heapArray<kj::byte>(entry.value)});
    }
    return result.releaseAsArray();
  }

  // Returns the real WebAssembly.Module for `id`, compiling it on first use.
  v8::Local<v8::WasmModuleObject> realModuleById(v8::Local<v8::Context> context, kj::StringPtr id) {
    v8::EscapableHandleScope scope(isolate);
    KJ_IF_SOME(cached, realModules.find(id)) {
      return scope.Escape(cached.Get(isolate));
    }
    kj::ArrayPtr<const kj::byte> bytes;
    KJ_IF_SOME(inl, inlineWasmBytes.find(id)) {
      bytes = inl;
    } else KJ_IF_SOME(source, sources.find(id)) {
      KJ_REQUIRE(source.kind == ModuleSource::WASM, "module is not a WebAssembly module", id);
      bytes = source.bytes;
    } else {
      KJ_FAIL_REQUIRE("unknown WebAssembly module id", id);
    }
    v8::TryCatch tryCatch(isolate);
    auto module = check(isolate, tryCatch,
        v8::WasmModuleObject::Compile(
            isolate, std::span<const uint8_t>(bytes.begin(), bytes.size())),
        id);
    realModules.insert(kj::str(id), v8::Global<v8::WasmModuleObject>(isolate, module));
    return scope.Escape(module);
  }

 private:
  v8::Isolate* isolate;
  kj::Maybe<PumpMessageLoop> pump;
  // Bundle readers; the embedder keeps the bundles alive for as long as the isolate exists.
  PyodideSnapshotArtifacts artifacts;
  Mode mode;
  kj::TimePoint startTime;
  kj::HashMap<kj::String, ModuleSource> sources;
  kj::HashMap<kj::String, v8::Global<v8::Module>> modules;
  kj::HashMap<kj::String, v8::Global<v8::WasmModuleObject>> realModules;
  kj::HashMap<kj::String, kj::Array<kj::byte>> inlineWasmBytes;
  kj::Maybe<jsg::WasmSnapshotRecorder> recorder;
  kj::Maybe<jsg::WasmSnapshotRestorer> restorer;
  uint nextInlineWasm = 0;

  void addBundle(jsg::Bundle::Reader bundle) {
    for (auto module: bundle.getModules()) {
      ModuleSource source;
      if (module.isSrc()) {
        source.kind = ModuleSource::ESM;
        source.bytes = module.getSrc();
      } else if (module.isData()) {
        source.kind = ModuleSource::DATA;
        source.bytes = module.getData();
      } else if (module.isWasm()) {
        source.kind = ModuleSource::WASM;
        source.bytes = module.getWasm();
      } else if (module.isJson()) {
        source.kind = ModuleSource::JSON;
        source.bytes = module.getJson().asBytes();
      } else {
        continue;
      }
      sources.upsert(kj::str(module.getName()), kj::mv(source), [](auto&, auto&&) {});
    }
  }

  void addShim(kj::StringPtr name, kj::StringPtr code) {
    ModuleSource source{.kind = ModuleSource::ESM, .bytes = code.asBytes()};
    sources.upsert(kj::str(name), kj::mv(source),
        [](auto& existing, auto&& replacement) { existing = kj::mv(replacement); });
  }

  v8::MaybeLocal<v8::Module> getModule(v8::Local<v8::Context> context, kj::StringPtr name) {
    v8::EscapableHandleScope scope(isolate);
    KJ_IF_SOME(cached, modules.find(name)) {
      return scope.Escape(cached.Get(isolate));
    }
    auto& source = KJ_UNWRAP_OR(sources.find(name), {
      throwTypeError(isolate, kj::str("Module not found in Pyodide snapshot harness: ", name));
      return {};
    });
    kj::String code;
    switch (source.kind) {
      case ModuleSource::ESM:
        code = kj::str(source.bytes.asChars());
        break;
      case ModuleSource::DATA:
        code = kj::str("export default globalThis.", kHarnessGlobal, ".data(\"", name, "\");");
        break;
      case ModuleSource::WASM:
        // The default export is a module stub; see snapshot-harness-shims.js for why no real
        // WebAssembly.Module may be reachable from JS.
        code = kj::str("const m = globalThis.", kHarnessGlobal, ".wasmModule(\"", name,
            "\");\nObject.setPrototypeOf(m, WebAssembly.Module.prototype);\nexport default m;");
        break;
      case ModuleSource::JSON:
        code = kj::str(
            "export default JSON.parse(globalThis.", kHarnessGlobal, ".text(\"", name, "\"));");
        break;
    }
    v8::ScriptOrigin origin(v8Str(isolate, name), 0, 0, false, -1, v8::Local<v8::Value>(), false,
        false, /* is_module = */ true);
    v8::ScriptCompiler::Source compilerSource(v8Str(isolate, code), origin);
    v8::Local<v8::Module> module;
    if (!v8::ScriptCompiler::CompileModule(isolate, &compilerSource).ToLocal(&module)) return {};
    modules.insert(kj::str(name), v8::Global<v8::Module>(isolate, module));
    return scope.Escape(module);
  }

  static v8::MaybeLocal<v8::Module> resolveModule(v8::Local<v8::Context> context,
      v8::Local<v8::String> specifier,
      v8::Local<v8::FixedArray> importAttributes,
      v8::Local<v8::Module> referrer) {
    auto isolate = v8::Isolate::GetCurrent();
    return from(isolate).getModule(context, toKj(isolate, specifier));
  }

  static v8::MaybeLocal<v8::Promise> importDynamically(v8::Local<v8::Context> context,
      v8::Local<v8::Data> hostDefinedOptions,
      v8::Local<v8::Value> resourceName,
      v8::Local<v8::String> specifier,
      v8::Local<v8::FixedArray> importAttributes) {
    auto isolate = v8::Isolate::GetCurrent();
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) return {};
    auto error = v8::Exception::Error(v8Str(isolate,
        kj::str("dynamic import() is not supported in the Pyodide snapshot harness: ",
            toKj(isolate, specifier))));
    (void)resolver->Reject(context, error);
    return resolver->GetPromise();
  }

  static void onPromiseReject(v8::PromiseRejectMessage message) {
    if (message.GetEvent() != v8::kPromiseRejectWithNoHandler) return;
    auto isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);
    auto value = message.GetValue();
    KJ_LOG(INFO, "unhandled promise rejection in Pyodide snapshot harness",
        value.IsEmpty() ? kj::str("(no value)") : toKj(isolate, value));
  }

  double nowMs() {
    return (kj::systemPreciseMonotonicClock().now() - startTime) / kj::MICROSECONDS / 1000.0;
  }

  struct TimerRun {
    int ran;           // timers that fired
    double nextDueMs;  // time until the next pending timer, or -1 if none
  };

  // Calls the shim's runTimers(), which fires every due timer.
  TimerRun runTimers(v8::Local<v8::Context> context) {
    v8::HandleScope scope(isolate);
    v8::TryCatch tryCatch(isolate);
    auto harness = check(isolate, tryCatch,
        context->Global()->Get(context, v8Str(isolate, kHarnessGlobal)), "harness global");
    KJ_REQUIRE(harness->IsObject());
    auto fn = check(isolate, tryCatch,
        harness.As<v8::Object>()->Get(context, v8Str(isolate, "runTimers")), "runTimers");
    KJ_REQUIRE(fn->IsFunction());
    auto result = check(isolate, tryCatch,
        fn.As<v8::Function>()->Call(context, harness, 0, nullptr), "runTimers()");
    KJ_REQUIRE(result->IsObject());
    auto object = result.As<v8::Object>();
    auto ran = check(isolate, tryCatch, object->Get(context, v8Str(isolate, "ran")), "ran");
    auto next = check(isolate, tryCatch, object->Get(context, v8Str(isolate, "next")), "next");
    return TimerRun{
      .ran = ran->IsInt32() ? ran.As<v8::Int32>()->Value() : 0,
      .nextDueMs = next->IsNumber() ? next.As<v8::Number>()->Value() : -1,
    };
  }

  v8::Local<v8::Private> moduleStubBrand() {
    return v8::Private::ForApi(isolate, v8Str(isolate, kModuleStubBrand));
  }

  v8::Local<v8::Object> newModuleStub(v8::Local<v8::Context> context, kj::StringPtr id) {
    auto stub = v8::Object::New(isolate);
    KJ_REQUIRE(stub->SetPrivate(context, moduleStubBrand(), v8Str(isolate, id)).FromMaybe(false),
        "branding module stub");
    return stub;
  }

  kj::Maybe<kj::String> moduleStubId(v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
    if (!value->IsObject()) return kj::none;
    v8::Local<v8::Value> id;
    if (!value.As<v8::Object>()->GetPrivate(context, moduleStubBrand()).ToLocal(&id) ||
        !id->IsString()) {
      return kj::none;
    }
    return toKj(isolate, id);
  }

  // ---- native callbacks (all registered as external references) ----
  //
  // Each is a thin wrapper around an *Impl: a KJ exception escaping a callback would unwind
  // through V8 frames and terminate the process, so the wrapper turns it into a JavaScript one.

  template <typename Body>
  static void guarded(const v8::FunctionCallbackInfo<v8::Value>& info, Body&& body) {
    KJ_IF_SOME(exception, kj::runCatchingExceptions(kj::fwd<Body>(body))) {
      auto isolate = info.GetIsolate();
      isolate->ThrowError(v8Str(isolate, exception.getDescription()));
    }
  }

  static void print(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { printImpl(info); });
  }
  static void printImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    int level = info.Length() >= 1 && info[0]->IsInt32() ? info[0].As<v8::Int32>()->Value() : 1;
    auto message = info.Length() >= 2 ? toKj(isolate, info[1]) : kj::str();
    if (level >= 2) {
      KJ_LOG(WARNING, "pyodide", message);
    } else {
      KJ_LOG(INFO, "pyodide", message);
    }
  }

  static void now(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { nowImpl(info); });
  }
  static void nowImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(from(info.GetIsolate()).nowMs());
  }

  static void randomFill(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { randomFillImpl(info); });
  }
  static void randomFillImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    if (info.Length() < 1 || !info[0]->IsArrayBufferView()) {
      return throwTypeError(isolate, "randomFill: expected an ArrayBufferView");
    }
    auto view = info[0].As<v8::ArrayBufferView>();
    auto bytes = static_cast<kj::byte*>(view->Buffer()->Data()) + view->ByteOffset();
    workerd::getEntropy(kj::arrayPtr(bytes, view->ByteLength()));
  }

  static void pyodideVersion(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { pyodideVersionImpl(info); });
  }
  static void pyodideVersionImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    info.GetReturnValue().Set(v8Str(isolate, from(isolate).artifacts.pyodideVersion));
  }

  static void compatibilityFlags(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { compatibilityFlagsImpl(info); });
  }
  static void compatibilityFlagsImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    info.GetReturnValue().Set(v8Str(isolate, from(isolate).artifacts.compatibilityFlagsJson));
  }

  static void data(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { dataImpl(info); });
  }
  static void dataImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    auto& self = from(isolate);
    auto name = toKj(isolate, info[0]);
    KJ_IF_SOME(source, self.sources.find(name)) {
      auto buffer = v8::ArrayBuffer::New(isolate, source.bytes.size());
      memcpy(buffer->Data(), source.bytes.begin(), source.bytes.size());
      return info.GetReturnValue().Set(buffer);
    }
    throwTypeError(isolate, kj::str("unknown data module: ", name));
  }

  static void text(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { textImpl(info); });
  }
  static void textImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    auto& self = from(isolate);
    auto name = toKj(isolate, info[0]);
    KJ_IF_SOME(source, self.sources.find(name)) {
      return info.GetReturnValue().Set(v8Str(isolate, kj::str(source.bytes.asChars())));
    }
    throwTypeError(isolate, kj::str("unknown module: ", name));
  }

  static void wasmModule(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { wasmModuleImpl(info); });
  }
  static void wasmModuleImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    auto& self = from(isolate);
    auto context = isolate->GetCurrentContext();
    auto name = toKj(isolate, info[0]);
    // Compile eagerly so that a bad module fails at import time, as it would in workerd.
    self.realModuleById(context, name);
    info.GetReturnValue().Set(self.newModuleStub(context, name));
  }

  static void compileWasm(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { compileWasmImpl(info); });
  }
  static void compileWasmImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    auto& self = from(isolate);
    auto context = isolate->GetCurrentContext();
    if (info.Length() < 1 || (!info[0]->IsArrayBufferView() && !info[0]->IsArrayBuffer())) {
      return throwTypeError(isolate, "WebAssembly.Module(): Argument 0 must be a buffer source");
    }
    kj::Array<kj::byte> bytes;
    if (info[0]->IsArrayBuffer()) {
      auto buffer = info[0].As<v8::ArrayBuffer>();
      bytes = kj::heapArray<kj::byte>(
          kj::arrayPtr(static_cast<const kj::byte*>(buffer->Data()), buffer->ByteLength()));
    } else {
      auto view = info[0].As<v8::ArrayBufferView>();
      bytes = kj::heapArray<kj::byte>(
          kj::arrayPtr(static_cast<const kj::byte*>(view->Buffer()->Data()) + view->ByteOffset(),
              view->ByteLength()));
    }
    // Identical bytes get the same id so that Pyodide's helper modules, which it compiles from
    // constant byte strings, are stored once.
    kj::String id;
    for (auto& entry: self.inlineWasmBytes) {
      if (entry.value == bytes) {
        id = kj::str(entry.key);
        break;
      }
    }
    if (id.size() == 0) {
      id = kj::str(kWasmModulePrefix, self.nextInlineWasm++);
      self.inlineWasmBytes.insert(kj::str(id), kj::mv(bytes));
    }
    v8::TryCatch tryCatch(isolate);
    v8::Local<v8::WasmModuleObject> module;
    {
      auto& stored = KJ_ASSERT_NONNULL(self.inlineWasmBytes.find(id));
      if (!v8::WasmModuleObject::Compile(
              isolate, std::span<const uint8_t>(stored.begin(), stored.size()))
               .ToLocal(&module)) {
        tryCatch.ReThrow();
        return;
      }
    }
    self.realModules.upsert(kj::str(id), v8::Global<v8::WasmModuleObject>(isolate, module),
        [&](auto& existing, auto&& replacement) { existing = kj::mv(replacement); });
    auto stub = self.newModuleStub(context, id);
    // Same prototype fix-up as the shim applies to bundle modules.
    auto realModuleCtor = context->Global()
                              ->Get(context, v8Str(isolate, "WebAssembly"))
                              .ToLocalChecked()
                              .As<v8::Object>()
                              ->Get(context, v8Str(isolate, "Module"))
                              .ToLocalChecked()
                              .As<v8::Object>();
    auto proto = realModuleCtor->Get(context, v8Str(isolate, "prototype")).ToLocalChecked();
    if (proto->IsObject()) (void)stub->SetPrototype(context, proto);
    info.GetReturnValue().Set(stub);
  }

  // Maps a module stub to the real WebAssembly.Module; passes real modules through.
  static void isModule(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { isModuleImpl(info); });
  }
  static void isModuleImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    auto& self = from(isolate);
    bool result = info.Length() >= 1 &&
        (info[0]->IsWasmModuleObject() ||
            self.moduleStubId(isolate->GetCurrentContext(), info[0]) != kj::none);
    info.GetReturnValue().Set(result);
  }

  static void realModule(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { realModuleImpl(info); });
  }
  static void realModuleImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    auto& self = from(isolate);
    auto context = isolate->GetCurrentContext();
    if (info.Length() < 1) return throwTypeError(isolate, "realModule: missing argument");
    if (info[0]->IsWasmModuleObject()) return info.GetReturnValue().Set(info[0]);
    KJ_IF_SOME(id, self.moduleStubId(context, info[0])) {
      return info.GetReturnValue().Set(self.realModuleById(context, id));
    }
    throwTypeError(isolate, "Argument is not a WebAssembly.Module");
  }

  static void instantiate(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { instantiateImpl(info); });
  }
  static void instantiateImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    auto& self = from(isolate);
    auto context = isolate->GetCurrentContext();
    if (info.Length() < 2 || !info[1]->IsObject()) {
      return throwTypeError(isolate, "instantiate: expected (module, imports)");
    }
    auto imports = info[1].As<v8::Object>();
    v8::Local<v8::WasmModuleObject> module;
    KJ_IF_SOME(stubId, self.moduleStubId(context, info[0])) {
      module = self.realModuleById(context, stubId);
      KJ_IF_SOME(rec, self.recorder) {
        return info.GetReturnValue().Set(rec.instantiate(context, stubId, module, imports));
      }
    } else if (self.recorder == kj::none && info[0]->IsWasmModuleObject()) {
      // A module compiled after the snapshot was restored, by the adopting worker: a shared
      // library. It can come from any context of the isolate.
      module = info[0].As<v8::WasmModuleObject>();
    } else {
      return throwTypeError(isolate, "instantiate: Argument 0 must be a WebAssembly.Module");
    }
    // Restored isolate: instances created after the snapshot are ordinary ones, importing the
    // real objects behind the stubs they were given.
    info.GetReturnValue().Set(self.getRestorer().instantiate(context, module, imports));
  }

  // Maps a snapshot trampoline to the real exported function; passes other values through.
  static void realFunction(const v8::FunctionCallbackInfo<v8::Value>& info) {
    guarded(info, [&]() { realFunctionImpl(info); });
  }
  static void realFunctionImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    if (info.Length() < 1) return throwTypeError(isolate, "realFunction: missing argument");
    v8::Local<v8::Value> real;
    if (jsg::resolveWasmSnapshotValue(isolate->GetCurrentContext(), info[0]).ToLocal(&real)) {
      info.GetReturnValue().Set(real);
    }
  }

  v8::Local<v8::Function> originalWasmConstructor(
      v8::Local<v8::Context> context, kj::StringPtr name) {
    // The shim replaced the constructor; the original is its prototype's `constructor`.
    auto wasm = context->Global()
                    ->Get(context, v8Str(isolate, "WebAssembly"))
                    .ToLocalChecked()
                    .As<v8::Object>();
    auto ctor = wasm->Get(context, v8Str(isolate, name)).ToLocalChecked().As<v8::Object>();
    auto proto = ctor->Get(context, v8Str(isolate, "prototype")).ToLocalChecked().As<v8::Object>();
    return proto->Get(context, v8Str(isolate, "constructor")).ToLocalChecked().As<v8::Function>();
  }

  friend kj::Array<intptr_t> pyodide::pyodideSnapshotExternalReferences();
};

}  // namespace

// =============================================================================================

kj::Array<intptr_t> pyodideSnapshotExternalReferences() {
  auto wasmRefs = jsg::getWasmSnapshotExternalReferences();
  const intptr_t own[] = {
    reinterpret_cast<intptr_t>(&Harness::print),
    reinterpret_cast<intptr_t>(&Harness::now),
    reinterpret_cast<intptr_t>(&Harness::randomFill),
    reinterpret_cast<intptr_t>(&Harness::pyodideVersion),
    reinterpret_cast<intptr_t>(&Harness::compatibilityFlags),
    reinterpret_cast<intptr_t>(&Harness::data),
    reinterpret_cast<intptr_t>(&Harness::text),
    reinterpret_cast<intptr_t>(&Harness::wasmModule),
    reinterpret_cast<intptr_t>(&Harness::compileWasm),
    reinterpret_cast<intptr_t>(&Harness::isModule),
    reinterpret_cast<intptr_t>(&Harness::realModule),
    reinterpret_cast<intptr_t>(&Harness::instantiate),
    reinterpret_cast<intptr_t>(&Harness::realFunction),
  };
  auto result = kj::heapArray<intptr_t>(wasmRefs.size() + kj::size(own) + 1);
  size_t i = 0;
  for (auto ref: wasmRefs) result[i++] = ref;
  for (auto ref: own) result[i++] = ref;
  result[i] = 0;
  return result;
}

PyodideSnapshot buildPyodideBaselineSnapshot(
    PumpMessageLoop pump, const PyodideSnapshotArtifacts& artifacts) {
  auto refs = pyodideSnapshotExternalReferences();
  std::shared_ptr<v8::ArrayBuffer::Allocator> allocator(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());

  v8::Isolate::CreateParams params;
  params.external_references = refs.begin();
  params.array_buffer_allocator_shared = allocator;
  v8::SnapshotCreator creator(params);
  auto isolate = creator.GetIsolate();

  PyodideSnapshot result;
  result.key = artifacts.key();
  {
    Harness harness(isolate, kj::mv(pump), artifacts, Harness::Mode::BUILD, nullptr,
        /*installIsolateCallbacks=*/true);
    v8::HandleScope handleScope(isolate);
    auto context = v8::Context::New(isolate);
    v8::Context::Scope contextScope(context);

    // The shims patch WebAssembly.Module/Instance, so the namespace must exist first.
    harness.getRecorder().exposeWebAssembly(context);
    harness.installGlobals(context);
    harness.loadPyodide(context);

    // Pyodide keeps its JS object references in externref tables inside the Wasm module, which
    // re-instantiation empties while Python's memory keeps indexing into them. Capture their
    // contents (the JS values themselves, which live on in the snapshot) the way Pyodide's own
    // memory snapshot does, so hydration can put them back.
    harness.runScript(context, "capture-hiwire.js", kCaptureHiwire);
    // Detach every live Wasm object and point Emscripten's heap views at the empty placeholder.
    harness.getRecorder().freeze(context);
    harness.runScript(context, "freeze.js",
        "pyodide._module.growMemory(0);\n"
        "globalThis.__pyodideSnapshotHarness.dropPromisingCache();");
    harness.releaseHandles();

    // A blob needs a default context, but it must be a different one: V8 serializes each
    // registered context separately and destructively adjusts some objects (typed arrays) in the
    // process, so registering the same context twice corrupts it. The Pyodide context is the one
    // added with AddContext(), which Context::FromSnapshot(0) restores.
    creator.SetDefaultContext(v8::Context::New(isolate));
    KJ_REQUIRE(creator.AddContext(context, harness.getRecorder().serializeCallback()) == 0);
    result.inlineWasm = harness.takeInlineWasm();

    // CreateBlob() must run while the recorder (whose callback it invokes) is alive.
    auto blob = creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
    KJ_REQUIRE(blob.data != nullptr && blob.raw_size > 0, "CreateBlob() produced no data");
    result.blob = kj::heapArray<kj::byte>(
        kj::arrayPtr(reinterpret_cast<const kj::byte*>(blob.data), blob.raw_size));
    delete[] blob.data;
  }
  return result;
}

// =============================================================================================

struct RestoredPyodide::Impl {
  v8::Isolate* isolate;
  Harness harness;
  v8::Global<v8::Context> context;

  Impl(v8::Isolate* isolate,
      const PyodideSnapshotArtifacts& artifacts,
      const PyodideSnapshot& snapshot,
      Options& options)
      : isolate(isolate),
        harness(isolate,
            kj::mv(options.pump),
            artifacts,
            Harness::Mode::RESTORE,
            snapshot.inlineWasm,
            options.installIsolateCallbacks) {
    KJ_REQUIRE(snapshot.key == artifacts.key(),
        "the Pyodide snapshot was built for a different Pyodide version or compatibility flags",
        snapshot.key, artifacts.key());
    v8::HandleScope handleScope(isolate);
    auto ctx = v8::Context::FromSnapshot(isolate, 0, harness.getRestorer().deserializeCallback())
                   .ToLocalChecked();
    context.Reset(isolate, ctx);
    v8::Context::Scope contextScope(ctx);

    // The context is reachable from the adopting worker's code through every object it holds,
    // so it gets the same restrictions as a worker context: no eval() or new Function() (as for
    // jsg contexts, `false` means "ask the isolate's callback", which the embedder sets), and
    // embedder data slots initialized so that jsg code that runs with this context current (the
    // dynamic import callback, say) reads an empty slot rather than past the end of the array.
    ctx->AllowCodeGenerationFromStrings(false);
    jsg::setAlignedPointerInEmbedderData(ctx, jsg::ContextPointerSlot::MAX_POINTER_SLOT, nullptr);

    auto count = harness.getRestorer().hydrate(ctx,
        [&](v8::Local<v8::Context> c, kj::StringPtr id) { return harness.realModuleById(c, id); },
        [&](v8::Local<v8::Context> c, kj::StringPtr id, v8::Local<v8::Object> instance) {
      // Emscripten runs the module's constructors on every fresh instance; they set up state the
      // memory image does not cover (Pyodide's externref tables).
      harness.callExportIfPresent(c, instance, "__wasm_call_ctors");
    });
    KJ_REQUIRE(count > 0, "snapshot contained no WebAssembly instances");
    // Re-point Emscripten's heap views at the restored memory (growMemory() with the current size
    // grows by zero pages and refreshes the views), then refill Pyodide's externref tables.
    harness.runScript(
        ctx, "hydrate.js", "pyodide._module.growMemory(pyodide._module.memory.buffer.byteLength);");
    harness.runScript(ctx, "restore-hiwire.js", kRestoreHiwire);

    // Hand the context's host services over to the embedder, then take away the harness natives
    // that only loading needed.
    KJ_IF_SOME(host, options.host) {
      harness.callHarnessFunction(ctx, "setHost", host.As<v8::Value>());
    }
    harness.callHarnessFunction(ctx, "seal", kj::none);
  }

  ~Impl() noexcept(false) {
    context.Reset();
  }
};

RestoredPyodide::RestoredPyodide(v8::Isolate* isolate,
    const PyodideSnapshotArtifacts& artifacts,
    const PyodideSnapshot& snapshot,
    Options options)
    : impl(kj::heap<Impl>(isolate, artifacts, snapshot, options)) {}

RestoredPyodide::~RestoredPyodide() noexcept(false) {}

v8::Local<v8::Context> RestoredPyodide::getContext() {
  return impl->context.Get(impl->isolate);
}

v8::Local<v8::Object> RestoredPyodide::getPyodide() {
  auto isolate = impl->isolate;
  v8::EscapableHandleScope scope(isolate);
  auto context = getContext();
  v8::Context::Scope contextScope(context);
  v8::TryCatch tryCatch(isolate);
  auto pyodide = check(isolate, tryCatch,
      context->Global()->Get(context, v8Str(isolate, "pyodide")), "pyodide global");
  KJ_REQUIRE(pyodide->IsObject(), "restored context has no pyodide object");
  return scope.Escape(pyodide.As<v8::Object>());
}

kj::String RestoredPyodide::evalToString(kj::StringPtr js) {
  auto isolate = impl->isolate;
  auto& harness = impl->harness;
  v8::HandleScope handleScope(isolate);
  auto context = getContext();
  v8::Context::Scope contextScope(context);
  auto result = harness.runScript(context, "eval.js", js);
  if (result->IsPromise()) {
    auto promise = result.As<v8::Promise>();
    harness.drain(context, promise);
    KJ_REQUIRE(promise->State() == v8::Promise::kFulfilled, "promise rejected",
        harness.describeValue(context, promise->Result()));
    result = promise->Result();
  } else {
    // Let any microtasks the script queued run before returning.
    isolate->PerformMicrotaskCheckpoint();
  }
  return toKj(isolate, result);
}

// =============================================================================================

struct RestoredPyodideContext::Impl {
  kj::Array<intptr_t> refs;
  std::shared_ptr<v8::ArrayBuffer::Allocator> allocator;
  v8::StartupData startupData;
  v8::Isolate* isolate;
  kj::Maybe<RestoredPyodide> restored;

  Impl(PumpMessageLoop pump,
      const PyodideSnapshotArtifacts& artifacts,
      const PyodideSnapshot& snapshot)
      : refs(pyodideSnapshotExternalReferences()),
        allocator(v8::ArrayBuffer::Allocator::NewDefaultAllocator()),
        startupData{reinterpret_cast<const char*>(snapshot.blob.begin()),
          static_cast<int>(snapshot.blob.size())} {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &startupData;
    params.external_references = refs.begin();
    params.array_buffer_allocator_shared = allocator;
    isolate = v8::Isolate::New(params);
    isolate->Enter();
    // An isolate left behind by a failure below would deadlock V8::Dispose() later.
    KJ_ON_SCOPE_FAILURE({
      restored = kj::none;
      isolate->Exit();
      isolate->Dispose();
    });
    v8::HandleScope handleScope(isolate);
    restored.emplace(isolate, artifacts, snapshot,
        RestoredPyodide::Options{.installIsolateCallbacks = true, .pump = kj::mv(pump)});
  }

  ~Impl() noexcept(false) {
    restored = kj::none;
    isolate->Exit();
    isolate->Dispose();
  }
};

RestoredPyodideContext::RestoredPyodideContext(PumpMessageLoop pump,
    const PyodideSnapshotArtifacts& artifacts,
    const PyodideSnapshot& snapshot)
    : impl(kj::heap<Impl>(kj::mv(pump), artifacts, snapshot)) {}

RestoredPyodideContext::~RestoredPyodideContext() noexcept(false) {}

kj::String RestoredPyodideContext::evalToString(kj::StringPtr js) {
  v8::HandleScope handleScope(impl->isolate);
  return KJ_ASSERT_NONNULL(impl->restored).evalToString(js);
}

// =============================================================================================
// Snapshot file format

namespace {
constexpr char kSnapshotFileMagic[8] = {'P', 'Y', 'V', '8', 'S', 'N', 'A', 'P'};
constexpr uint32_t kSnapshotFileVersion = 2;

class ByteWriter {
 public:
  void raw(const void* p, size_t n) {
    buf.addAll(kj::arrayPtr(reinterpret_cast<const kj::byte*>(p), n));
  }
  void u32(uint32_t v) {
    raw(&v, sizeof(v));
  }
  void u64(uint64_t v) {
    raw(&v, sizeof(v));
  }
  void bytes(kj::ArrayPtr<const kj::byte> b) {
    u64(b.size());
    raw(b.begin(), b.size());
  }
  kj::Array<kj::byte> finish() {
    return buf.releaseAsArray();
  }

 private:
  kj::Vector<kj::byte> buf;
};

class ByteReader {
 public:
  explicit ByteReader(kj::ArrayPtr<const kj::byte> data): data(data) {}
  void raw(void* p, size_t n) {
    KJ_REQUIRE(n <= data.size() - pos, "truncated Pyodide V8 snapshot file");
    memcpy(p, data.begin() + pos, n);
    pos += n;
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
  kj::Array<kj::byte> bytes() {
    auto n = u64();
    KJ_REQUIRE(n <= data.size() - pos, "truncated Pyodide V8 snapshot file");
    auto result = kj::heapArray<kj::byte>(n);
    raw(result.begin(), n);
    return result;
  }
  bool atEnd() {
    return pos == data.size();
  }

 private:
  kj::ArrayPtr<const kj::byte> data;
  size_t pos = 0;
};
}  // namespace

kj::Array<kj::byte> serializePyodideSnapshot(const PyodideSnapshot& snapshot) {
  ByteWriter w;
  w.raw(kSnapshotFileMagic, sizeof(kSnapshotFileMagic));
  w.u32(kSnapshotFileVersion);
  w.bytes(snapshot.key.asBytes());
  w.bytes(snapshot.blob);
  w.u32(static_cast<uint32_t>(snapshot.inlineWasm.size()));
  for (auto& wasm: snapshot.inlineWasm) {
    w.bytes(wasm.id.asBytes());
    w.bytes(wasm.bytes);
  }
  return w.finish();
}

kj::Arc<LoadedPyodideSnapshot> LoadedPyodideSnapshot::fromFile(kj::ArrayPtr<const kj::byte> bytes) {
  return fromSnapshot(deserializePyodideSnapshot(bytes));
}

kj::Arc<LoadedPyodideSnapshot> LoadedPyodideSnapshot::fromSnapshot(PyodideSnapshot snapshot) {
  LoadedPyodideSnapshot loaded;
  auto artifact = kj::uniqueArc<jsg::SnapshotArtifact>();
  // SnapshotArtifact releases the blob with delete[], so it gets its own allocation; the
  // PyodideSnapshot kept alongside gives its copy up rather than hold a second one.
  auto blob = new char[snapshot.blob.size()];
  memcpy(blob, snapshot.blob.begin(), snapshot.blob.size());
  artifact->blob = v8::StartupData{blob, static_cast<int>(snapshot.blob.size())};
  snapshot.blob = nullptr;
  loaded.snapshot = kj::mv(snapshot);
  artifact->externalReferences = pyodideSnapshotExternalReferences();
  loaded.artifact = kj::Arc<jsg::SnapshotArtifact>(kj::mv(artifact));
  return kj::arc<LoadedPyodideSnapshot>(kj::mv(loaded));
}

PyodideSnapshot deserializePyodideSnapshot(kj::ArrayPtr<const kj::byte> bytes) {
  ByteReader r(bytes);
  char magic[sizeof(kSnapshotFileMagic)];
  r.raw(magic, sizeof(magic));
  KJ_REQUIRE(
      memcmp(magic, kSnapshotFileMagic, sizeof(magic)) == 0, "not a Pyodide V8 snapshot file");
  KJ_REQUIRE(r.u32() == kSnapshotFileVersion, "unsupported Pyodide V8 snapshot file version");
  PyodideSnapshot result;
  result.key = kj::str(r.bytes().asChars());
  result.blob = r.bytes();
  auto count = r.u32();
  auto inlineWasm = kj::heapArrayBuilder<PyodideSnapshot::InlineWasm>(count);
  for (uint32_t i = 0; i < count; i++) {
    auto id = r.bytes();
    inlineWasm.add(PyodideSnapshot::InlineWasm{
      .id = kj::str(id.asChars()),
      .bytes = r.bytes(),
    });
  }
  result.inlineWasm = inlineWasm.finish();
  KJ_REQUIRE(r.atEnd(), "trailing bytes in Pyodide V8 snapshot file");
  return result;
}

}  // namespace workerd::api::pyodide
