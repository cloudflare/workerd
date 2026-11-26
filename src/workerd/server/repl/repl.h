// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ half of `workerd repl`: the functions the command line's REPL (cli/repl/) calls through
// its cxx bridge (cli/repl/bridge.rs). Rust reads and edits the input, evaluates it, holds the
// JavaScript API and serves the configs the input loads; C++ sets up V8 and the session's I/O,
// builds the workers input is evaluated in (repl-worker.h) and the factories the configs' servers
// run on, and renders values with the runtime's `inspect()`.
//
// Do not include the generated bridge header here: the bridge includes this header.

#include <workerd/io/io-channels.h>
#include <workerd/io/io-context/ffi.h>
#include <workerd/rust/jsg/ffi.h>

#include <rust/cxx.h>

#include <kj/async.h>
#include <kj/memory.h>
#include <kj/refcount.h>

namespace kj {
class EntropySource;
class Filesystem;
class HttpHeaderTable;
class MonotonicClock;
class Network;
class Timer;
}  // namespace kj

namespace kj_rs_tokio {
struct TokioAsyncIoContext;
}  // namespace kj_rs_tokio

namespace workerd::server {

class ProcessState;
struct SubrequestChannel;
class WorkerFactory;

// The REPL session's process-wide resources, which its workers and the servers of the configs it
// loads are built from.
struct ReplServerResources {
  // The process's V8 and autogates (factory/per-process.h), which the first session starts.
  const ProcessState& process;
  kj::Timer& timer;
  const kj::MonotonicClock& monotonicClock;
  kj::Network& network;
  kj::EntropySource& entropySource;
  kj::Filesystem& fs;
};

}  // namespace workerd::server

namespace workerd::server::repl {

struct ReplProcessOptions;
struct ReplWorkerOptions;

// The session's resources, as Rust holds them. The session outlives every holder: Rust drains
// and drops its environments before the session.
class ReplResources final: public kj::Refcounted {
 public:
  explicit ReplResources(const ReplServerResources& resources): resources(resources) {}

  ReplServerResources resources;
};

// What the servers of an environment's configs are built from: the session's resources, and the
// environment's header table, which keys the headers of the requests the REPL sends, and
// `--experimental`. The environment closes its servers before it ends, so these outlive them.
class ReplFactorySource final: public kj::Refcounted {
 public:
  ReplFactorySource(const ReplServerResources& resources,
      const kj::HttpHeaderTable& headerTable,
      bool experimental)
      : resources(resources),
        headerTable(headerTable),
        experimental(experimental) {}

  ReplServerResources resources;
  const kj::HttpHeaderTable& headerTable;
  bool experimental;
};

// A REPL session's I/O: the timer, network and filesystem of the event loop it was created on,
// and the process's state. Created on the event loop's thread, and used only there.
class ReplSession {
 public:
  class Impl;

  explicit ReplSession(kj::Own<Impl> impl);
  ~ReplSession() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ReplSession);

  Impl& getImpl() {
    return *impl;
  }

 private:
  kj::Own<Impl> impl;
};

// A worker that input is evaluated in, and that never receives an event (repl-worker.h).
class ReplWorker {
 public:
  class Impl;

  explicit ReplWorker(kj::Own<Impl> impl);
  ~ReplWorker() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ReplWorker);

  Impl& getImpl() {
    return *impl;
  }
  const Impl& getImpl() const {
    return *impl;
  }

 private:
  kj::Own<Impl> impl;
};

// See the bridge (cli/repl/bridge.rs).
kj::Own<ReplSession> repl_session_new(
    kj_rs_tokio::TokioAsyncIoContext& loop, const ReplProcessOptions& options);
kj::Rc<ReplResources> repl_session_resources(ReplSession& session);
kj::Own<ReplWorker> repl_worker_new(
    const ReplResources& resources, const ReplWorkerOptions& options);
kj::Own<io_context::IncomingRequest> repl_worker_new_request(ReplWorker& worker);
::rust::Str repl_worker_input_name(const ReplWorker& worker);
kj::Rc<ReplFactorySource> repl_worker_factory_source(ReplWorker& worker);
bool repl_inspect(
    v8::Isolate* isolate, ::workerd::rust::jsg::Local value, bool colors, ::rust::String& result);
kj::Rc<IoChannelFactory::SubrequestChannel> repl_channel_into_kj(
    ::rust::Box<SubrequestChannel> channel);
kj::Own<WorkerFactory> repl_new_factory(
    const ReplFactorySource& source, ::rust::Vec<uint64_t> config);

}  // namespace workerd::server::repl
