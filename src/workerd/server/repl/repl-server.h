// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/server/repl/repl-api.h>
#include <workerd/util/strong-bool.h>

#include <rust/cxx.h>

#include <capnp/common.h>
#include <kj/async.h>
#include <kj/compat/http.h>
#include <kj/filesystem.h>
#include <kj/function.h>
#include <kj/list.h>
#include <kj/refcount.h>
#include <kj/time.h>
#include <kj/timer.h>

namespace workerd::jsg {
class V8System;
}  // namespace workerd::jsg

namespace workerd::server {

class WorkerFactory;
struct InProcessServer;

// What no two servers of a REPL session may hold at once: the directory of each of their durable
// Durable Object namespaces. Two servers over one such directory would run two instances of each
// of its Durable Objects on one database. One for the session, whose environments share it.
class ReplServerClaims {
 public:
  // What one server holds, until it is destroyed.
  class Claim {
   public:
    Claim(ReplServerClaims& registry, kj::String path, kj::Array<kj::String> held);
    ~Claim() noexcept(false);
    KJ_DISALLOW_COPY_AND_MOVE(Claim);

   private:
    ReplServerClaims& registry;
    // The config of the server, as the user named it.
    kj::String path;
    // Each a phrase, such as "stores Durable Objects in /storage/counter".
    kj::Array<kj::String> held;
    kj::ListLink<Claim> link;

    friend class ReplServerClaims;
  };

  // Claims `held` for the server of the config `path`. Throws a JSG error if a claim other than
  // `except` holds one of them.
  kj::Own<Claim> claim(
      kj::StringPtr path, kj::Array<kj::String> held, kj::Maybe<const Claim&> except = kj::none);

 private:
  kj::List<Claim, &Claim::link> claims;
};

// What the server of a loaded config is built from: the REPL session's process-wide resources.
struct ReplServerResources {
  jsg::V8System& v8System;
  kj::Timer& timer;
  const kj::MonotonicClock& monotonicClock;
  kj::Network& network;
  kj::EntropySource& entropySource;
  kj::Filesystem& fs;
  ReplServerClaims& claims;
};

// The files a config was loaded from, watched for changes.
class ReplConfigWatch {
 public:
  virtual ~ReplConfigWatch() noexcept(false) = default;

  // Resolves once one of the files has changed and changes have stopped arriving for a moment, as
  // `workerd serve --watch` waits for them.
  virtual kj::Promise<void> whenChanged() = 0;
};

// Loads configs as encoded `Config` messages. `workerd repl` loads them as `workerd serve` does
// (repl.c++, over cli/repl/session.rs).
class ReplConfigLoader {
 public:
  virtual ~ReplConfigLoader() noexcept(false) = default;

  // The config at `path`, with `constName` for a Cap'n Proto config. With `watch`, from
  // newWatch(), the load watches each file it reads. Throws a JSG error describing the problems if
  // the config does not load; `watch` still watches the files read until then.
  virtual kj::Array<capnp::word> load(kj::StringPtr path,
      kj::Maybe<kj::StringPtr> constName,
      kj::Maybe<ReplConfigWatch&> watch) = 0;

  // A watch of no files yet.
  virtual kj::Own<ReplConfigWatch> newWatch() = 0;
};

// What every server the REPL loads is built with.
struct ReplServerContext {
  ReplServerResources resources;
  // Keys the headers of the requests the REPL sends.
  const kj::HttpHeaderTable& replHeaderTable;
  ReplConfigLoader& loader;
  // `--experimental`.
  bool experimental;
  // ReplEnvironmentOptions::beforeServerClose.
  kj::Maybe<kj::Function<kj::Promise<void>()>&> beforeClose;
};

// Whether a reload prints its outcome rather than failing with it.
WD_STRONG_BOOL(ReportReloadOutcome);

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
//
// Reloading loads the config again and builds a new Rust server on it. The old one is revoked and
// closed as closing would, and then the new one serves in its place: two servers of one config at
// once would run two instances of each of its Durable Objects. A config that doesn't load, or
// whose server would store Durable Objects where another server does, leaves the old server
// running. A server that doesn't start leaves nothing serving until a reload starts one. Reloads
// run one at a time, in the order asked for. A watched config is reloaded whenever a file the last
// load read changes, so one that failed is reloaded once it is fixed; the outcome is printed to
// stderr.
//
// Loading a config refuses it if its server would store Durable Objects where another server of
// the session does (ReplServerClaims), until that server's close is done.
class ReplConfigServer final: public ReplServerHost, public kj::Refcounted {
 public:
  // Loads the config `source` names and starts serving it. Resolves once every socket listens.
  // Rejects with a JSG error listing the problems if it does not load, if it has errors, or if the
  // server does not start; the server is then closed, and a failure to close is listed with them.
  static kj::Promise<kj::Own<ReplConfigServer>> load(
      ReplServerContext context, ReplConfigSource source);

  ReplConfigServer(ReplServerContext context,
      ReplConfigSource source,
      ::rust::Box<InProcessServer> server,
      kj::Array<kj::String> warnings,
      kj::Own<ReplServerClaims::Claim> claim);
  ~ReplConfigServer() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ReplConfigServer);

  kj::Own<ReplServerHost> addRef() override {
    return kj::addRef(*this);
  }
  kj::StringPtr getPath() const override {
    return source.path;
  }
  bool isClosed() const override {
    return closing != kj::none;
  }
  // Whether the close has finished, or failed; isClosed() is true from when it begins.
  bool isCloseDone() const {
    return closeDone;
  }
  bool isWatching() const override {
    return source.watch && !isClosed();
  }
  kj::ArrayPtr<const kj::String> getWarnings() const override {
    return warnings;
  }
  kj::Rc<IoChannelFactory::SubrequestChannel> service(kj::StringPtr name,
      kj::Maybe<kj::StringPtr> entrypoint,
      kj::Maybe<Frankenvalue> props) override;
  kj::Rc<IoChannelFactory::SubrequestChannel> socket(kj::StringPtr name) override;
  kj::Rc<IoChannelFactory::SubrequestChannel> durableObject(
      const ReplActorAddress& address) override;
  kj::Promise<void> reload() override;
  kj::Promise<void> close() override;

 private:
  ReplServerContext context;
  ReplConfigSource source;
  // The running server, which owns its factory; none once closed, while a reload replaces it, and
  // after a reload whose new server did not start.
  kj::Maybe<::rust::Box<InProcessServer>> server;
  kj::Array<kj::String> warnings;
  // A reload has closed the server and has yet to start the new one.
  bool replacing = false;

  // The last reload asked for, settled or not. Never rejects.
  kj::ForkedPromise<void> reloading;
  // Waits for a watched file to change, then asks for a reload; none if the config isn't watched,
  // or once closing has begun.
  kj::Maybe<kj::Promise<void>> watching;
  // Set by the close as it ends, even if it is destroyed unfinished: declared before `closing`,
  // which holds it.
  bool closeDone = false;
  // What the server holds, or would; none once nothing runs. Released by the close as it ends,
  // like `closeDone`.
  kj::Maybe<kj::Own<ReplServerClaims::Claim>> claim;
  kj::Maybe<kj::ForkedPromise<void>> closing;

  // Asks for a reload after the one asked for last; the reload, which fails if it does unless
  // `report` prints its outcome instead.
  kj::ForkedPromise<void> queueReload(ReportReloadOutcome report);
  kj::Promise<void> reloadAfter(kj::Promise<void> previous, ReportReloadOutcome report);
  // Replaces the server with one of the config as it is now, `watch` watching the files the load
  // reads.
  kj::Promise<void> replaceServer(kj::Maybe<ReplConfigWatch&> watch);
  // Asks for a reload once a file `watch` watches changes.
  void watchFor(kj::Own<ReplConfigWatch> watch);
  kj::Promise<void> closeServer();

  InProcessServer& requireOpen();
};

}  // namespace workerd::server
