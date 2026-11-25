// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ half of `workerd repl`: the functions the command line's REPL (cli/repl/) calls through
// its cxx bridge (cli/repl/bridge.rs). Rust reads and edits the input, splits it into inputs,
// decides how each may compile, and serves the configs the input loads (cli/repl/server.rs); C++
// owns V8 and evaluates (repl-environment.h), and builds the factories the configs' servers run
// on.
//
// Do not include the generated bridge header here: the bridge includes this header.

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

namespace workerd::jsg {
class V8System;
}  // namespace workerd::jsg

namespace workerd::server {

class WorkerFactory;

// The REPL session's process-wide resources, which the servers of the configs it loads are built
// from.
struct ReplServerResources {
  jsg::V8System& v8System;
  kj::Timer& timer;
  const kj::MonotonicClock& monotonicClock;
  kj::Network& network;
  kj::EntropySource& entropySource;
  kj::Filesystem& fs;
};

}  // namespace workerd::server

namespace workerd::server::repl {

struct ReplOptions;
struct ReplCandidate;
struct ReplEvaluation;
struct ServerRegistry;

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

// A REPL session: V8, the I/O the loaded configs use, and the environment input runs in. Created
// on the event loop's thread, and used only there.
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

// See the bridge (cli/repl/bridge.rs).
kj::Own<ReplSession> repl_session_new(kj_rs_tokio::TokioAsyncIoContext& loop,
    const ReplOptions& options,
    ::rust::Box<ServerRegistry> registry);
kj::Promise<ReplEvaluation> repl_evaluate(
    ReplSession& session, ::rust::Vec<ReplCandidate> candidates);
kj::Promise<::rust::Vec<::rust::String>> repl_property_names(
    ReplSession& session, ::rust::String base);
kj::Promise<bool> repl_drain(ReplSession& session);
kj::Own<WorkerFactory> repl_new_factory(
    const ReplFactorySource& source, ::rust::Slice<const uint64_t> config);

}  // namespace workerd::server::repl
