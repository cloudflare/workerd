// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "snapshot-harness.h"

#include <workerd/api/pyodide-runtime-bundle.embed.h>
#include <workerd/jsg/setup.h>

#include <libplatform/libplatform.h>
#include <pyodide/pyodide_static.capnp.h>

#include <capnp/serialize.h>
#include <kj/filesystem.h>
#include <kj/test.h>

#include <cstdlib>

namespace workerd::api::pyodide {
namespace {

constexpr kj::StringPtr kPyodideVersion = "314.0.6"_kj;

// Writes the blob next to the test outputs when running under Bazel, or to $PYODIDE_SNAPSHOT_OUT
// if set, so it can be inspected or reused.
void maybeWriteSnapshot(const PyodideSnapshot& snapshot) {
  kj::StringPtr path;
  if (auto env = getenv("PYODIDE_SNAPSHOT_OUT"); env != nullptr && *env != '\0') {
    path = env;
  } else if (auto dir = getenv("TEST_UNDECLARED_OUTPUTS_DIR"); dir != nullptr && *dir != '\0') {
    static kj::String buf;
    buf = kj::str(dir, "/pyodide-baseline.snapshot");
    path = buf;
  } else {
    return;
  }
  auto fs = kj::newDiskFilesystem();
  auto file = fs->getRoot().openFile(
      fs->getCurrentPath().evalNative(path), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
  file->writeAll(snapshot.blob);
  KJ_LOG(WARNING, "wrote Pyodide baseline snapshot", path, snapshot.blob.size());
}

KJ_TEST("baseline Pyodide load survives a V8 context snapshot") {
  auto platform = jsg::defaultPlatform(4);
  // Extra V8 flags for debugging, e.g. PYODIDE_SNAPSHOT_V8_FLAGS="--verify-heap --expose-gc".
  kj::Vector<kj::String> flagStorage;
  kj::Vector<kj::StringPtr> flags;
  if (auto env = getenv("PYODIDE_SNAPSHOT_V8_FLAGS"); env != nullptr) {
    kj::StringPtr rest = env;
    while (rest.size() > 0) {
      auto end = rest.findFirst(' ').orDefault(rest.size());
      if (end > 0) flagStorage.add(kj::str(rest.first(end)));
      rest = rest.slice(kj::min(end + 1, rest.size()));
    }
    for (auto& f: flagStorage) flags.add(f);
  }
  jsg::V8System v8System(*platform, flags, platform.get());

  auto bundleWords =
      kj::arrayPtr(reinterpret_cast<const capnp::word*>(PYODIDE_RUNTIME_BUNDLE.begin()),
          PYODIDE_RUNTIME_BUNDLE.size() / sizeof(capnp::word));
  capnp::ReaderOptions options;
  options.traversalLimitInWords = kj::maxValue;
  capnp::FlatArrayMessageReader runtimeReader(bundleWords, options);

  PyodideSnapshotArtifacts artifacts{
    .staticBundle = PYODIDE_BUNDLE,
    .runtimeBundle = runtimeReader.getRoot<jsg::Bundle>(),
    .pyodideVersion = kj::str(kPyodideVersion),
    .compatibilityFlagsJson = kj::str(R"({"python_workers":true,"python_workers_development":true,)"
                                      R"("python_workers_20250116":true,"enable_weak_ref":true})"),
  };

  auto pump = [&platform](v8::Isolate* isolate) {
    return v8::platform::PumpMessageLoop(platform.get(), isolate);
  };
  auto snapshot = jsg::runInV8Stack(
      [&](jsg::V8StackScope&) { return buildPyodideBaselineSnapshot(pump, artifacts); });
  KJ_EXPECT(snapshot.blob.size() > 1024 * 1024, snapshot.blob.size());
  KJ_LOG(WARNING, "Pyodide baseline snapshot", snapshot.blob.size(), snapshot.inlineWasm.size());
  maybeWriteSnapshot(snapshot);

  // Round-trip through the file format, as workerd does.
  auto serialized = serializePyodideSnapshot(snapshot);
  snapshot = deserializePyodideSnapshot(serialized);

  jsg::runInV8Stack([&](jsg::V8StackScope&) {
    RestoredPyodideContext restored(pump, artifacts, snapshot);

    // The snapshot stops at the baseline; the embedder finishes the bootstrap (python.ts
    // finishPythonBootstrap). The one part of that which Pyodide's scheduler needs is the signal
    // buffer that clearSignals() writes before running a task.
    restored.evalToString("pyodide._module.Py_EmscriptenSignalBuffer = new Uint8Array(1); 'ok'");

    // The interpreter works without any further initialization.
    KJ_EXPECT(restored.evalToString("pyodide.runPython('1 + 1')") == "2");
    KJ_EXPECT(restored.evalToString("pyodide.version") == kPyodideVersion);
    KJ_EXPECT(restored.evalToString("pyodide.runPython('import sys; sys.version.split()[0]')")
                  .startsWith("3.14"));

    // The baseline imports are already in sys.modules: this is the state the snapshot carried.
    KJ_EXPECT(restored.evalToString(
                  "pyodide.runPython('import sys; all(m in sys.modules for m in "
                  "[\"asyncio\", \"zipfile\", \"tarfile\", \"typing\", \"inspect\"])')") == "true");

    // Memory-heavy work and JS<->Python round trips through the trampolines.
    KJ_EXPECT(
        restored.evalToString("pyodide.runPython('sum(range(1_000_000))')") == "499999500000");
    KJ_EXPECT(restored.evalToString("pyodide.runPython('import re; re.sub(\"a+\", \"b\", "
                                    "\"caaat\")')") == "cbt");
    KJ_EXPECT(restored.evalToString("pyodide.runPython('import js; js.Math.max(3, 7)')") == "7");
    KJ_EXPECT(restored.evalToString(
                  "pyodide.runPython('[len(bytes(range(256)) * 4096)]').toJs()[0]") == "1048576");

    // JSPI was available while the snapshot was built and still works after restore: run_sync
    // suspends the Python stack until the JS promise settles. runPythonAsync enters Python
    // through a promising wrapper, which is what allows the suspension.
    KJ_EXPECT(restored.evalToString("pyodide._module.jspiSupported") == "true");
    // Plain JSPI on a module instantiated after restore, independent of Pyodide.
    KJ_EXPECT(restored.evalToString(R"(
      (async () => {
        // (module (import "m" "f" (func $f (param i32) (result i32)))
        //         (func (export "g") (param i32) (result i32) local.get 0 call $f))
        const bytes = new Uint8Array([0,97,115,109,1,0,0,0,1,6,1,96,1,127,1,127,2,7,1,1,109,1,102,0,0,
          3,2,1,0,7,5,1,1,103,0,1,10,8,1,6,0,32,0,16,0,11]);
        const suspending = new WebAssembly.Suspending(async (x) => { await null; return x + 1; });
        const { instance } = await WebAssembly.instantiate(bytes, { m: { f: suspending } });
        const g = WebAssembly.promising(instance.exports.g);
        return await g(41);
      })()
    )") == "42");
    KJ_EXPECT(restored.evalToString(R"(
      pyodide.runPythonAsync(`
        from pyodide.ffi import run_sync
        import js
        run_sync(js.Promise.resolve(41)) + 1
      `)
    )") == "42");

    // Python -> JS callback through a PyProxy.
    KJ_EXPECT(
        restored.evalToString("pyodide.runPython('lambda f: f(20) + 1')(x => x * 2)") == "41");
  });
}

}  // namespace
}  // namespace workerd::api::pyodide
