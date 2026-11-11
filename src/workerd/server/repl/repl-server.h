// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/server/repl/repl-api.h>

#include <rust/cxx.h>

#include <capnp/common.h>
#include <kj/async.h>
#include <kj/compat/http.h>
#include <kj/filesystem.h>
#include <kj/refcount.h>
#include <kj/time.h>
#include <kj/timer.h>

namespace workerd::jsg {
class V8System;
}  // namespace workerd::jsg

namespace workerd::server {

class WorkerFactory;
struct InProcessServer;

// What the server of a loaded config is built from: the REPL session's process-wide resources.
struct ReplServerResources {
  jsg::V8System& v8System;
  kj::Timer& timer;
  const kj::MonotonicClock& monotonicClock;
  kj::Network& network;
  kj::EntropySource& entropySource;
  kj::Filesystem& fs;
};

// A config `workerd.loadConfig()` loaded, served in-process by the Rust server (in_process.rs) as
// `workerd serve` serves a config, on a worker factory of its own over the session's V8 and event
// loop. Its sockets listen on in-process (loopback) names instead of their addresses, so that the
// REPL can load configs whose ports are taken, or load one config twice. The process-wide parts of
// a config, its V8 flags and autogates, are ignored with a warning: the REPL's process has its own.
// Outbound connections reach the network, as `workerd serve`'s do.
//
// The channels to its services and sockets are the Rust server's (in_process/host.rs), which
// rekeys headers between the REPL's header table and the factory's. Closing the server revokes
// every channel it handed out, and every request to its workers still in progress, before the
// server takes its factory apart, so that a JavaScript Fetcher or Response the REPL still holds
// does not keep the factory alive.
class ReplConfigServer final: public ReplServerHost, public kj::Refcounted {
 public:
  // Starts serving the config `config` (an encoded `Config` message) loaded from `path`, for
  // requests from an IoContext whose headers `replHeaderTable` keys. Resolves once every socket
  // listens. Rejects with a JSG error listing the config errors if there are any or the server
  // does not start; the server is then closed.
  static kj::Promise<kj::Own<ReplConfigServer>> start(const ReplServerResources& resources,
      const kj::HttpHeaderTable& replHeaderTable,
      kj::String path,
      kj::ArrayPtr<const capnp::word> config,
      bool experimental);

  ReplConfigServer(kj::String path, ::rust::Box<InProcessServer> server);
  ~ReplConfigServer() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ReplConfigServer);

  kj::Own<ReplServerHost> addRef() override {
    return kj::addRef(*this);
  }
  kj::StringPtr getPath() const override {
    return path;
  }
  bool isClosed() const override {
    return closing != kj::none;
  }
  kj::ArrayPtr<const kj::String> getWarnings() const override {
    return warnings;
  }
  kj::Rc<IoChannelFactory::SubrequestChannel> service(kj::StringPtr name,
      kj::Maybe<kj::StringPtr> entrypoint,
      kj::Maybe<Frankenvalue> props) override;
  kj::Rc<IoChannelFactory::SubrequestChannel> socket(kj::StringPtr name) override;
  kj::Promise<void> close() override;

 private:
  kj::String path;
  // The server, which owns the factory; none once closing has begun.
  kj::Maybe<::rust::Box<InProcessServer>> server;
  kj::Array<kj::String> warnings;

  kj::Maybe<kj::ForkedPromise<void>> closing;

  // Starts the run; returns the config errors, or the failure to start if it reported none.
  kj::Promise<kj::Array<kj::String>> serve();
  kj::Promise<void> closeServer();

  InProcessServer& requireOpen();
};

}  // namespace workerd::server
