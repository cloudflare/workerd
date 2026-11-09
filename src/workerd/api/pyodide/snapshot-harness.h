// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Builds a V8 context snapshot in which Pyodide has been loaded to the same point at which
// workerd takes its Pyodide *linear memory* snapshot (the "baseline": interpreter initialized and
// the baseline stdlib modules imported), and restores such a snapshot.
//
// The snapshot is taken in a bare V8 context, not in a workerd worker context: V8 cannot serialize
// jsg's API objects. The Pyodide runtime bundle only needs a handful of Web platform globals to
// load, which snapshot-harness-shims.js provides, and the C++-backed builtin modules it imports
// are replaced by small JS shims with the answers a baseline load needs. WebAssembly instances
// are carried through the snapshot by jsg::WasmSnapshotRecorder; see jsg/wasm-snapshot.h for
// what that does and does not preserve.
//
// JSPI (WebAssembly.Suspending / promising) is available while the snapshot is built, so Pyodide
// enables its stack-switching support; `WebAssembly.promising()` wrappers are re-created lazily
// against the re-instantiated module (see snapshot-harness-shims.js).

#include <workerd/jsg/modules.capnp.h>
#include <workerd/jsg/snapshot.h>

#include <v8.h>

#include <kj/array.h>
#include <kj/common.h>
#include <kj/function.h>
#include <kj/string.h>

namespace workerd::api::pyodide {

struct PyodideSnapshotArtifacts {
  // The `pyodide_static` bundle (PYODIDE_BUNDLE): the TypeScript glue and `.py` data modules.
  jsg::Bundle::Reader staticBundle;
  // The per-version runtime bundle (`pyodide.capnp.bin`): emscriptenSetup, pyodide.asm.wasm and
  // python_stdlib.zip.
  jsg::Bundle::Reader runtimeBundle;
  // The Pyodide version the runtime bundle was built from, as reported to the glue (e.g.
  // "314.0.6").
  kj::String pyodideVersion;
  // JSON object of the compatibility flags the baseline load depends on (see
  // getPythonCompatibilityFlagsJson()), which the snapshot bakes in.
  kj::String compatibilityFlagsJson;

  PyodideSnapshotArtifacts clone() const {
    return {
      .staticBundle = staticBundle,
      .runtimeBundle = runtimeBundle,
      .pyodideVersion = kj::str(pyodideVersion),
      .compatibilityFlagsJson = kj::str(compatibilityFlagsJson),
    };
  }

  // Identifies what a snapshot built from these artifacts is good for.
  kj::String key() const {
    return kj::str(pyodideVersion, ' ', compatibilityFlagsJson);
  }
};

struct PyodideSnapshot {
  // PyodideSnapshotArtifacts::key() of the artifacts the snapshot was built from.
  kj::String key;

  // The V8 startup snapshot blob. Its single embedder context (index 0 for
  // v8::Context::FromSnapshot) holds the loaded Pyodide.
  kj::Array<kj::byte> blob;

  // WebAssembly modules that were compiled from bytes at build time rather than taken from a
  // bundle (Pyodide's tiny helper modules), keyed by the id recorded in the snapshot. The restore
  // side needs these to re-instantiate them.
  struct InlineWasm {
    kj::String id;
    kj::Array<kj::byte> bytes;
  };
  kj::Array<InlineWasm> inlineWasm;
};

// File format for a PyodideSnapshot: the blob and the inline Wasm modules.
kj::Array<kj::byte> serializePyodideSnapshot(const PyodideSnapshot& snapshot);
PyodideSnapshot deserializePyodideSnapshot(kj::ArrayPtr<const kj::byte> bytes);

// A snapshot shared by every isolate started from it, in the two forms they need: the
// PyodideSnapshot for RestoredPyodide, and a jsg::SnapshotArtifact (the blob plus the external
// references) to create the isolate with, via jsg::FinalizedSnapshot.
struct LoadedPyodideSnapshot {
  PyodideSnapshot snapshot;
  kj::Arc<jsg::SnapshotArtifact> artifact;

  static kj::Arc<LoadedPyodideSnapshot> fromSnapshot(PyodideSnapshot snapshot);
  static kj::Arc<LoadedPyodideSnapshot> fromFile(kj::ArrayPtr<const kj::byte> bytes);
};

// Native callback addresses that both the building and the restoring isolate must pass in
// v8::Isolate::CreateParams::external_references. Null-terminated.
kj::Array<intptr_t> pyodideSnapshotExternalReferences();

// Runs the V8 platform's pending foreground tasks for an isolate once; returns whether any ran.
// For V8's default platform this is `v8::platform::PumpMessageLoop(platform, isolate)`.
using PumpMessageLoop = kj::Function<bool(v8::Isolate*)>;

// Loads Pyodide to the baseline and serializes the context. Creates its own isolate; must run in
// a process whose V8 flags match those of the isolates that will restore the snapshot, as V8
// refuses snapshots built under different flags.
PyodideSnapshot buildPyodideBaselineSnapshot(
    PumpMessageLoop pump, const PyodideSnapshotArtifacts& artifacts);

// Pyodide restored from a PyodideSnapshot into an existing isolate, as a context of its own. The
// isolate must have been created with `snapshot.blob` as its `snapshot_blob` and with
// pyodideSnapshotExternalReferences() in its `external_references`. The caller holds the isolate
// lock and is inside a handle scope for every method; the object must be destroyed under the
// lock before the isolate is disposed.
class RestoredPyodide final {
 public:
  struct Options {
    // Whether to set the isolate-level callbacks (dynamic import, promise rejection, microtask
    // policy) the harness uses when it owns the isolate. Off when restoring into an isolate that
    // an embedder (jsg) has already configured.
    bool installIsolateCallbacks = false;
    // Used only by evalToString() to wait for promises.
    kj::Maybe<PumpMessageLoop> pump;
    // The global object whose `console`, `performance`, timers and `crypto` the restored Pyodide
    // uses from then on, in place of the placeholders it was loaded with; typically the global of
    // the worker context that adopts it, so that Python observes the worker's clock and writes to
    // its console. If none, the placeholders stay: the clock is the system's and output goes to
    // the log, which is only fit for tests.
    kj::Maybe<v8::Local<v8::Object>> host;
  };

  RestoredPyodide(v8::Isolate* isolate,
      const PyodideSnapshotArtifacts& artifacts,
      const PyodideSnapshot& snapshot,
      Options options);
  ~RestoredPyodide() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(RestoredPyodide);

  // The restored context.
  v8::Local<v8::Context> getContext();

  // The restored `pyodide` object (Pyodide's public API, with `_module` for the Emscripten
  // module). Lives in the restored context; usable from any context of the isolate.
  v8::Local<v8::Object> getPyodide();

  // Evaluates `js` as a classic script in the restored context, drives the event loop until the
  // result (awaited if it is a promise) settles, and returns String(result). Throws a KJ
  // exception carrying the JS error message if the script throws or the promise rejects.
  // Requires `Options::pump`.
  kj::String evalToString(kj::StringPtr js);

 private:
  struct Impl;
  kj::Own<Impl> impl;
};

// For tests: an isolate of its own, created from a PyodideSnapshot and restored. `artifacts` and
// `snapshot` must outlive the object.
class RestoredPyodideContext final {
 public:
  RestoredPyodideContext(PumpMessageLoop pump,
      const PyodideSnapshotArtifacts& artifacts,
      const PyodideSnapshot& snapshot);
  ~RestoredPyodideContext() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(RestoredPyodideContext);

  kj::String evalToString(kj::StringPtr js);

 private:
  struct Impl;
  kj::Own<Impl> impl;
};

}  // namespace workerd::api::pyodide
