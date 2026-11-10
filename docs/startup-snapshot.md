# Startup Snapshots

How workerd evaluates a Worker's top-level code once, in a throwaway isolate, and starts the
Worker from a V8 startup snapshot of the result.

For the JSG reference material (the `jsgSnapshotStateless` / `snapshotClone()` contract, the
`SnapshotArtifact` fields), see [src/workerd/jsg/README.md](../src/workerd/jsg/README.md)
§Startup Snapshot. For the declarations, see `src/workerd/jsg/snapshot.h` and the snapshot
section of `src/workerd/jsg/setup.h`.

## Overview

Starting a Worker normally means compiling its modules, evaluating the main module graph and
running every top-level statement, inside the isolate that will serve requests. With a startup
snapshot the same work happens in a **zygote**: a second, throwaway `Worker` whose isolate was
created by a `v8::SnapshotCreator`. When the zygote's top level has run, its context is serialized
into a blob, and the real Worker's isolate is created from that blob. Its context comes out of
`v8::Context::New()` with the module graph already evaluated and the top-level state in place, so
the real Worker skips evaluation entirely.

Everything the real Worker needs from the zygote travels in a `jsg::SnapshotArtifact`
(`src/workerd/jsg/snapshot.h`): the V8 blob, the external-reference table it was built against,
and the native side of what the blob cannot carry (see [What is transferred](#what-is-transferred)).

The feature is gated and best-effort. Any failure in the zygote means the Worker starts the way it
always has; see [Failures](#failures).

## When it runs

`factory_new_worker()` in `src/workerd/server/factory/worker-factory.c++` runs the zygote when
all of these hold:

- the `STARTUP_SNAPSHOT` autogate is on (`workerd-autogate-startup-snapshot` in a config's
  top-level `autogates = [...]`, or `WORKERD_ALL_AUTOGATES=1`);
- no inspector is configured: neither `--inspector-addr` nor the
  `enable_nodejs_inspector_local_dev` flag. The inspector attaches to the Worker's isolate, and
  top-level code has to run there for DevTools to see its console output and stop at its
  breakpoints;
- `supportsStartupSnapshot()` accepts the Worker: ES-modules syntax (not a service-worker
  script), no Wasm modules, not a Python Worker, the legacy module registry (no
  `new_module_registry`), the `typescript_implemented_streams` flag, and only bindings of kinds
  that transfer (`WorkerdApi::isBindingTransferableThroughSnapshot()`; see step 3 below). The C++
  streams implementation leaves a pending `pull()` as a C++ promise continuation reachable from
  the context, which `CreateBlob()` cannot serialize; the TypeScript implementation keeps it in
  JavaScript. A dynamically loaded Worker (`dynamicSource`), whose `env` arrives as capabilities
  rather than channel numbers, never qualifies.

## The zygote (PREPARE_SNAPSHOT)

`jsg::IsolateBase::isPreparingSnapshot()` is true in the zygote's isolate. In order:

1. `makeIsolate(<name>-snapshot, InspectorPolicy::DISALLOW, MutableSnapshot{artifact})`. The
   `IsolateBase` constructor creates the isolate through a `v8::SnapshotCreator`, and every
   `registerExternalReference()` call from then on lands in `artifact.externalReferences`.
2. `newContext()` (`jsg::Isolate::newContextWithWrapper`, `setup.h`) creates the context and
   calls `installRestoreTimeGlobalsForZygote()`: V8 leaves some properties out of a context built
   in a serializer-enabled isolate and adds them when the context is restored
   (`Error.stackTraceLimit`, `WebAssembly`, `SharedArrayBuffer`, the Iterator helpers behind
   shipped flags). The zygote installs `Error.stackTraceLimit` as the data property V8 would, and
   the others as fail-closed accessors that throw and record the use, so top-level code sees a
   normal context and cannot bake a wrong feature test in, even when it catches the error. See the
   comment block above `RESTORE_TIME_GLOBALS` in `setup.h`.
3. The zygote `Worker::Script` and `Worker` are built exactly as the real ones would be, with an
   `ErrorReporter` of their own, bindings included. A binding object is a description (a channel
   number and names) that every request resolves through its own IoContext, so creating it does
   no I/O, and it transfers with `env` like any other live object (`snapshotClone()` on
   `Fetcher`, `KvNamespace`, `DurableObjectNamespace`, ...). `supportsStartupSnapshot()` keeps a
   Worker out of the pipeline when one of its bindings is of a kind that cannot transfer
   (`WorkerdApi::isBindingTransferableThroughSnapshot()`: `cryptoKey`, `memoryCache`, `wrapped`).
4. The module graph is evaluated: this is where top-level code runs. Console output it produces
   is held back (`Worker::Isolate::deferConsoleLine()`); see [Console output](#console-output).
5. Still in the `Worker` constructor, before the context's handle scope closes:
   - `settleFinalizationRegistriesForSnapshot()` runs the FinalizationRegistry cleanups the top
     level left due, the way the event loop would after its job: microtask checkpoint, WeakRef
     kept objects cleared, a full GC, then every task V8 posted, round after round until a GC
     posts nothing more (a bounded number of rounds; beyond it the zygote fails). The callbacks
     run without an IoContext, as they do at request time, under the startup limits; an
     exception in one is reported as an uncaught exception and does not fail the zygote. This is
     the last JavaScript the zygote runs: everything recorded from here on could otherwise be
     changed behind the record's back, and the live-object record below keys wrappers by a
     cppgc shim address that is unique only while nothing allocates;
   - the `env` object and the main module's namespace are pinned into the snapshot
     (`pinIntoSnapshot()`, indices `BINDINGS_SCOPE_SNAPSHOT_INDEX` and
     `MAIN_MODULE_NAMESPACE_SNAPSHOT_INDEX`);
   - `removeRestoreTimeGlobalsBeforeSnapshot()` records what the top level left of
     `Error.stackTraceLimit` and removes every restore-time property, so that V8 can add them
     back on restore. A fail-closed property the top level used (whether or not it caught the
     error), a property it defined in their place, or a holder it made non-extensible, fails the
     zygote here;
   - the global scope's event listeners are recorded (`EventTarget::forEachSnapshotListener()`,
     one pinned identity object per listener) into `artifact.listenerRecords`;
   - `stashPerIsolateBootstrapForSnapshot()` bundles the per-isolate bootstrap's JS-held state
     into one object and pins it (`artifact.bootstrapStateIndex`), the last of the record steps.

   Only a top level that ran to completion is recorded. If it failed (a parse error, an uncaught
   exception, a startup limit: `impl->permanentException` is set), none of the record steps run
   and the `Worker` constructor throws instead of going on to step 6: `CreateBlob()` on a
   half-built context would only risk a V8 fatal. The context is disposed with the script.
6. The zygote's global scope is cleared, the script's module context handle dropped, and
   `IsolateBase::createSnapshotBlob()` runs. Its steps are numbered in `setup.c++`:
   1. `bakeModuleRegistryIntoSnapshot()`: every materialized module handle of the legacy registry
      is pinned and recorded in `artifact.moduleRecords`; the handles are reset.
   2. `WrappablesSerializeMetadata::recordLiveWrappables()`: every live JSG object is recorded
      while it still exists, by its tag alone for a stateless type, as a `snapshotClone()`
      otherwise (`artifact.liveWrappables`), keyed by its CppgcShim address. The metadata roots
      every recorded shim, so no shim allocated later can take a recorded address.
   3. `HeapTracer::destroyLiveWrappableInstances()`: every live JSG object is destroyed, which
      resets the member handles `CreateBlob()`'s global-handle check would otherwise reject.
      A full GC then collects what that released. Registered objects that only C++ handles
      reached die here, and `CreateBlob()`'s serializer rejects an isolate whose registries hold
      cleared cells, so the task runner is pumped under a `DisallowJavascriptExecutionScope`:
      V8's cleanup task pops each cell and the callback call throws instead of running. No
      JavaScript runs past step 2. Any such cell fails the zygote: those objects are C++-held
      state the snapshot does not carry, and their cleanups must not be dropped silently.
   4. Isolate-level handles are reset (the opaque template, `workerEnvObj`, `workerExportsObj`,
      template handles drained from Rust JSG).
   5. `bakeConstructorTemplatesIntoSnapshot()`: every materialized JSG resource constructor
      template is pinned at isolate level and recorded, by position, in
      `artifact.constructorTemplateIndices`.
   6. Struct-type handles are reset.
   7. The default-context handle is reset and `SnapshotCreator::CreateBlob()` runs. V8 calls
      `serializeInternalFields()` for each wrapper's tag field and `serializeApiWrapperCallback()`
      for each wrapper's shim pointer; the latter writes the object's `liveWrappables` index as
      the payload. Afterwards `WrappablesSerializeMetadata::dropUnreachableWrappables()` raises
      what the callbacks found (an unknown shim, a type with no transfer) and drops the clones of
      objects that were recorded but not reachable at `CreateBlob()`.
7. Back in `factory_new_worker()`: if the zygote threw or reported errors, the artifact is
   discarded. Otherwise `extractSnapshotArtifact()` hands it over, the deferred console lines are
   printed, and the real isolate is created from it.

## The real Worker (START_FROM_SNAPSHOT)

`jsg::IsolateBase::isStartingFromSnapshot()` is true in the real Worker's isolate when a
`FinalizedSnapshot` was passed to `makeIsolate()`. The `kj::Arc<SnapshotArtifact>` stays alive as
long as the isolate does. In order:

1. `IsolateBase` creates the isolate with `CreateParams::snapshot_blob` and the artifact's
   external references; the `jsg::Isolate` constructor then runs
   `restoreConstructorTemplatesFromSnapshot()`, pairing each recorded index with the template
   slot at the same position. The enumeration order is fixed at compile time, and the producer
   and the consumer are the same binary.
2. `newContext()` runs `prepareWrappableDeserializeMetadata()`: for each `liveWrappables` slot it
   builds the object the restored wrapper will be attached to (a stateless type's constructor,
   or a second `snapshotClone()` of the stored clone) and pre-allocates its CppgcShim, since the
   deserializer callback runs under V8's `DisallowGarbageCollection` and may not allocate.
3. `v8::Context::New()` deserializes the context. `deserializeInternalFields()` restores the
   wrapper-tag marker of every wrapper; `deserializeApiWrapperCallback()` links each wrapper to
   the shim pre-allocated for its payload's slot (`HeapTracer::linkRestoredWrapper()`). Then
   `releaseSlotsWithChecks()` verifies every serialized slot was linked exactly once, and
   `replayRestoreTimeGlobalsAfterLoad()` re-applies the recorded `Error.stackTraceLimit` state
   on top of the defaults V8 just installed.
4. The `Worker::Script` constructor rebuilds the per-isolate bootstrap's C++ state from the
   pinned object instead of running the bootstrap scripts (`restoreBootstrapState()`), replays
   each recorded listener through the global's own `addEventListener`, and seeds the module
   registry: `addSnapshotSeed()` per `moduleRecords` entry, then `compileModules()`, which
   registers a bundle ES module the zygote instantiated around its baked instance
   (`tryTakeSnapshotModule()`) and still registers the lazily compilable builtins the zygote
   never touched, then `applySnapshotSeeds()`. Runtime `import()`, `require()` and the
   `Buffer`/`process` globals therefore resolve to the instances the top level evaluated against.
5. The `Worker` constructor takes `env` from the snapshot (`extractOnceFromSnapshot()`), complete
   with its bindings: `factory_new_worker()` compiles no bindings for a Worker started from a
   snapshot, while `ctx.exports` is still built for it afterwards, by `worker_set_ctx_exports()`.
   It takes the main module's namespace from the snapshot instead of resolving the main module.
   No top-level code runs.

## What is transferred

| State                                        | Mechanism                                                   | `SnapshotArtifact` field          |
| -------------------------------------------- | ----------------------------------------------------------- | --------------------------------- |
| The context: module graph, closures, objects | V8 blob                                                     | `blob`                            |
| C++ callback addresses the blob refers to    | `registerExternalReference()` table                         | `externalReferences`              |
| `env` with its bindings, main module namespace | context-level `AddData`, fixed indices                     | `blob` (indices in `snapshot.h`)  |
| JSG resource constructor templates           | isolate-level `AddData`, by position                        | `constructorTemplateIndices`      |
| Live JSG objects' native side                | tag only (stateless) or `snapshotClone()`; wrapper payload = slot index | `liveWrappables`      |
| Per-isolate bootstrap state                  | one pinned object                                           | `bootstrapStateIndex`             |
| `Error.stackTraceLimit` as the top level left it | pinned record, replayed after restore                   | `restoreTimeGlobalsIndex`         |
| Materialized module-registry entries         | pinned handles, seeded into the new registry                | `moduleRecords`                   |
| Global-scope event listeners                 | pinned identity objects, re-registered on load              | `listenerRecords`                 |

Not transferred, by design: `ctx.exports` (built for the real Worker after the zygote has run),
the context global's own `ServiceWorkerGlobalScope` (`newContext()` attaches a fresh one; its
listener map is what `listenerRecords` restores), and wrappers unreachable at `CreateBlob()`.

FinalizationRegistry cleanups the top level leaves due run in the zygote (step 5 above) and
their effects are part of the snapshot, as if a collection had happened right after the top
level. A registered object reachable only through a C++ object's handle is a different case: it
is not carried by the snapshot, and rather than run or drop its cleanup, the zygote fails.

## Making a type transferable

A JSG resource type whose instance can be alive when top-level evaluation ends needs one of:

- `static constexpr bool jsgSnapshotStateless = true;` when any fresh instance is
  interchangeable with any other (`SubtleCrypto`, `Navigator`, `TextEncoder`, ...). The snapshot
  records only the wrapper's tag; the worker started from it attaches an instance built by the
  type's `Lock&` constructor, or its default constructor.
- an override of `Wrappable::snapshotClone()` returning a deep, isolate-independent copy
  (`Headers`, `URL`, `FileSystemModule`): no V8 handles, no `jsg::Ref` members, no IoContext.
  It is called in the zygote on the live original, and once more per started worker on the
  stored copy.

Neither may throw: see the contracts on `Object::jsgSnapshotStateless` (`jsg.h`) and
`Wrappable::snapshotClone()` (`wrappable.h`). A type with neither makes every Worker whose top
level leaves such an object reachable fall back; `dropUnreachableWrappables()` names the type in
the zygote's error
(`live wrapper types that neither set jsgSnapshotStateless nor implement snapshotClone() ...`).

A binding type's `snapshotClone()` copies the binding's description (its channel number and
names) and returns none when the object is bound to one IoContext instead (a live channel or
factory, a stub minted per request) or has registered a connect override on the zygote's global
scope (`ctx.mapVirtualHost()`), which the restored Worker's fresh global scope would lack. A
subclass that does not override `snapshotClone()` fails the zygote rather than transferring as
its base type.

## Failures

Two outcomes are possible for a Worker that qualifies:

- **The Worker does not qualify.** `supportsStartupSnapshot()` says no: an inspector, a
  service-worker script, Wasm modules, Python, the new module registry, C++ streams, or a binding
  of a kind that cannot transfer (`cryptoKey`, `memoryCache`, `wrapped`, logged at INFO as
  `startup snapshot skipped: a binding of this kind cannot be transferred`), or a dynamically
  loaded Worker. No zygote runs.
- **The zygote fails.** A KJ exception while compiling, evaluating, recording or serializing: a
  top-level exception or parse error (the `Worker` constructor throws before anything is
  recorded), an unclonable type reachable from the context such as an `AbortController` or an
  RPC method taken off a stub (`const { ping } = env.RPC`), a restore-time global the top level
  redefined, a listener registered with a `signal` option, FinalizationRegistry cleanups that
  keep dooming new objects or that become due only once the zygote's objects are destroyed, ...
  Or an error reported to the zygote's `ErrorReporter` without an exception (a module that does
  not transpile, for example). `factory_new_worker()` logs it at INFO (`startup snapshot
  skipped: the zygote Worker threw`, with the exception's description, or `... failed to start`,
  with the reported errors) and the Worker starts without a snapshot, evaluating its top level
  itself. This is the common case for a Worker the pipeline does not yet cover and costs one
  wasted evaluation.
- **The restore fails.** A check in the START_FROM_SNAPSHOT path (an artifact slot never linked,
  a pinned module with no registry entry, a stateless constructor or `snapshotClone()` that
  throws) surfaces as the Worker's own startup failure; there is no second attempt without the
  snapshot. These checks guard invariants the zygote established, so a failure here is a bug in
  the pipeline rather than in the Worker.

V8 fatal errors (`CHECK` failures) abort the process and cannot be caught at either stage. The
one known way for a Worker's top level to cause one, a duplicate of a property V8 adds on restore,
is what the restore-time-globals handling exists to prevent.

## Console output

Under `ConsoleMode::STDOUT` the zygote's top-level `console.*` calls and runtime warnings do not
reach stdout/stderr directly: `handleLog()` and `Worker::Isolate::logWarning()` hand them to
`deferConsoleLine()` while `isPreparingSnapshot()`. When the snapshot is used,
`flushDeferredConsoleOutput()` prints them in order; when the Worker falls back, they are dropped
with the zygote's isolate and the Worker's own evaluation prints them. Either way each line appears
once.

## Debugging

- The INFO log `startup snapshot skipped` names the Worker and the reason. `workerd test` prints
  INFO logs; `workerd serve` needs `--verbose`.
- `Unknown external reference` from `CreateBlob()` means the context reaches a C++ callback whose
  address was not passed to `registerExternalReference()`. JSG registers its own callbacks; a new
  `v8::Function::New`/`FunctionTemplate::New` call site with a native callback needs to register
  it too.
- `-c dbg` builds with the shared pointer-compression cage trip V8's read-only-heap checksum
  check when a snapshot is consumed; use a fastbuild binary, or a multi-cage V8 build, to run the
  pipeline under a debugger.

## Tests

| Test                                                                 | Covers                                                                     |
| -------------------------------------------------------------------- | -------------------------------------------------------------------------- |
| `src/workerd/jsg/snapshot-test.c++`                                  | restore-time globals: install, removal, rejection cases; live-wrappable round trip (stateless, cloned, dropped and untransferable types), `dropUnreachableWrappables()` and `releaseSlotsWithChecks()` findings; FinalizationRegistry cleanups settled before the record, rejected after it |
| `src/workerd/api/tests/startup-snapshot-globals-test`                | top-level code touching `Error.stackTraceLimit`, `WebAssembly`, `Iterator` |
| `src/workerd/api/tests/startup-snapshot-listeners-test`              | top-level `addEventListener()` across the snapshot, `signal` fallback      |
| `src/workerd/api/tests/startup-snapshot-modules-test`                | module identity across the snapshot (ESM, JSON, CJS, `node:buffer`)        |
| `src/workerd/api/tests/startup-snapshot-wrappables-test`             | live objects (Headers, URL, TextEncoder, crypto, navigator, bindings), GC and a FinalizationRegistry cleanup across the snapshot; unclonable, `URLSearchParams`, `node:fs` and ineligible cases fall back |
| `src/workerd/api/tests/startup-snapshot-bindings-test`               | bindings captured at module scope (service stub, Durable Object namespace and ID) used at request time; an RPC method taken off a stub, `wrapped` and `cryptoKey` fall back |
| `src/workerd/server/tests/startup-snapshot-console`                  | console output printed once; no zygote under an inspector; a top level that throws gets no snapshot and its startup error is reported once |

Each `wd_test` runs the pipeline in its `@all-autogates` variant and the plain Worker in the `@`
variant, and asserts the same observable behavior for both. A case Worker learns which path it
took at request time from `startedFromStartupSnapshot()` on `workerd:unsafe`; asked at module
scope, the answer would be the zygote's and be baked into the snapshot.
