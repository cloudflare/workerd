// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// What a workerd process sets up once, before any worker factory exists, and the factories built
// on it. The command line (bootstrap.h) starts the process with `ProcessState::start()` and builds
// one factory with `newWorkerFactory()`; a host that serves several configs builds one factory per
// config on the same process.
//
// Do not include a generated bridge header here: the server crate's bridge includes this header.

#include <workerd/server/factory/worker-factory.h>

#include <rust/cxx.h>

#include <kj/array.h>
#include <kj/memory.h>
#include <kj/refcount.h>
#include <kj/string.h>

namespace workerd {
class AsyncTraceWriter;
}  // namespace workerd

namespace workerd::jsg {
class V8System;
}  // namespace workerd::jsg

namespace workerd::server {

class LoopIo;

// The process's tracing, allocator, autogates, test and bench modes, and V8: everything that
// lives as long as the process serves and that every factory shares. Started at most once per
// process (V8 cannot start again once it is torn down), from any thread; usable from any thread.
//
// Each factory holds a reference, so the state outlives every factory built on it, and with it
// every isolate and context: V8 is torn down, the --async-trace file ended and the perfetto trace
// flushed only after the last factory is gone and the last other reference dropped.
class ProcessState final: public kj::AtomicRefcounted {
 public:
  struct Options {
    // --perfetto-trace: the file, and the categories (comma-separated; empty for the default).
    kj::Maybe<kj::String> perfettoTracePath;
    kj::String perfettoTraceCategories;
    // --async-trace: the file. The factories' isolates write to it.
    kj::Maybe<kj::String> asyncTracePath;
    // The gates to enable, each `workerd-autogate-<name>`; others are ignored.
    kj::Array<kj::String> autogates;
    kj::Array<kj::String> v8Flags;
    // Every gate, over `autogates`.
    bool allAutogates = false;
    // Info logging: uncaught exceptions are logged at that level.
    bool infoLogging = false;
    bool predictable = false;
    bool gcStress = false;
    bool benchMode = false;
  };

  // Sets up the process as `options` say, in this order: the perfetto session, the async trace
  // file, tcmalloc's background thread (with heap sampling off), the autogates, the test and bench
  // modes, and V8 with its flags (V8 reads the test modes as it starts). Throws if the process
  // already started V8, whether or not that state is still alive. A start that fails before V8
  // started may be tried again; what it set up by then (autogates, modes) stays set until the
  // next start sets it again.
  static kj::Arc<ProcessState> start(const Options& options);

  class Impl;
  explicit ProcessState(kj::Own<Impl> impl);
  ~ProcessState() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ProcessState);

  jsg::V8System& getV8System() const {
    return v8System;
  }
  kj::Maybe<const AsyncTraceWriter&> getAsyncTraceWriter() const;

  kj::Arc<ProcessState> addRef() const {
    return addRefToThis();
  }

 private:
  kj::Own<Impl> impl;
  jsg::V8System& v8System;
};

// A worker factory over `config`, an encoded `Config` message the factory owns for its run, on
// `process`'s V8 and `io` (loop-io.h). The factory holds a reference to both, dropping the
// process's before the I/O's, and its workers write to the process's async trace file, if it has
// one.
kj::Own<WorkerFactory> newWorkerFactory(const ProcessState& process,
    kj::Rc<LoopIo> io,
    kj::Own<WorkerFactory::Options> options,
    ::rust::Vec<uint64_t> config);

}  // namespace workerd::server
