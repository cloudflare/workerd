// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The JavaScript API of `workerd repl`: the `workerd` global (ReplControl), the handle it
// returns for each config it loads (ReplServer), and the handle of each environment it starts
// (ReplEnvironmentHandle).
//
// The types are in the workerd API isolate's type list (workerd-api-isolate.h), so the REPL's
// isolate can wrap them, but only the REPL's isolate has a `workerd` global through which to reach
// them. They are not in api/rtti.c++: they are not part of the Workers API and have no TypeScript
// types.
//
// workerd-api depends on this header, and the REPL's implementation depends on workerd-api, so the
// types reach that implementation through the abstract ReplHost, ReplServerHost and
// ReplEnvironmentHost.

#include <workerd/api/http.h>
#include <workerd/api/unsafe.h>
#include <workerd/io/frankenvalue.h>
#include <workerd/io/io-channels.h>
#include <workerd/jsg/jsg.h>

#include <kj/async.h>

namespace workerd::server {

// A Durable Object of a loaded config: of the class `className` that the config's service
// `service` exports, named by `name` (as `idFromName()` names it) or by `id` (as `idFromString()`
// takes it, for a durable class). One of `name` and `id` is given. The strings are used only
// during the call they are passed to.
struct ReplActorAddress {
  kj::StringPtr service;
  kj::StringPtr className;
  kj::Maybe<kj::StringPtr> name;
  kj::Maybe<kj::StringPtr> id;
};

// A config the REPL loaded and serves in-process (repl-server.h). Shared between the REPL and the
// JavaScript handles to it; closing the server, not the last handle's garbage collection, ends it.
class ReplServerHost {
 public:
  virtual ~ReplServerHost() noexcept(false) = default;

  virtual kj::Own<ReplServerHost> addRef() = 0;

  // The config file, as the user named it.
  virtual kj::StringPtr getPath() const = 0;
  virtual bool isClosed() const = 0;
  // Whether a change to a file the config was loaded from reloads it.
  virtual bool isWatching() const = 0;
  // The warnings the server reported as it loaded the config, or last reloaded it.
  virtual kj::ArrayPtr<const kj::String> getWarnings() const = 0;

  // A channel to the config's service `name`: its `entrypoint` (the default if none), with `props`
  // if given. Requests through the channel fail once the server is closed. Throws a JSG error if
  // there is no such service or entrypoint, or the server is closed.
  virtual kj::Rc<IoChannelFactory::SubrequestChannel> service(
      kj::StringPtr name, kj::Maybe<kj::StringPtr> entrypoint, kj::Maybe<Frankenvalue> props) = 0;

  // A channel that sends HTTP requests to the config's socket `name`, as a client connecting to it
  // would. Throws a JSG error if there is no such socket, or the server is closed.
  virtual kj::Rc<IoChannelFactory::SubrequestChannel> socket(kj::StringPtr name) = 0;

  // A channel to the Durable Object `address` names, as a binding to its class reaches it. Throws
  // a JSG error if there is no such service or class, the address is not valid for the class, or
  // the server is closed.
  virtual kj::Rc<IoChannelFactory::SubrequestChannel> durableObject(
      const ReplActorAddress& address) = 0;

  // Loads the config again and serves it in place of what was loaded before, whose channels then
  // fail. Rejects with a JSG error listing the problems, leaving the server as it was, if the
  // config does not load, or with one if the server is closed.
  virtual kj::Promise<void> reload() = 0;

  // Drains the server and takes it apart. Idempotent.
  virtual kj::Promise<void> close() = 0;
};

// A config to load.
struct ReplConfigSource {
  // The config file, as the user named it: a Cap'n Proto config, or a `wrangler.json` /
  // `wrangler.jsonc`, relative to the working directory.
  kj::String path;
  // The constant that holds a Cap'n Proto config, as for `workerd serve`.
  kj::Maybe<kj::String> constName;
  // Reload whenever a file the config was loaded from changes.
  bool watch = false;
};

// Another environment like the REPL's, with compatibility settings of its own, which
// `workerd.environment()` started. Shared between the REPL and the JavaScript handles to it;
// closing it, not the last handle's garbage collection, ends it.
class ReplEnvironmentHost {
 public:
  virtual ~ReplEnvironmentHost() noexcept(false) = default;

  virtual kj::Own<ReplEnvironmentHost> addRef() = 0;

  // The compatibility date the environment has, today's if none was given.
  virtual kj::StringPtr getCompatibilityDate() const = 0;
  // The compatibility flags it was given.
  virtual kj::ArrayPtr<const kj::String> getCompatibilityFlags() const = 0;
  virtual bool isClosed() const = 0;

  // Evaluates `input` there as the REPL evaluates a line typed at it: what the REPL would print,
  // the rendered result or "Uncaught " and the exception. Rejects with a JSG error if the
  // environment is closed, or closes meanwhile.
  virtual kj::Promise<kj::String> evaluate(kj::String input) = 0;

  // Closes the environment's servers and ends its JavaScript. Idempotent.
  virtual kj::Promise<void> close() = 0;
};

// The compatibility settings of an environment to start.
struct ReplEnvironmentSettings {
  // Today if none.
  kj::Maybe<kj::String> compatibilityDate;
  kj::Array<kj::String> compatibilityFlags;
};

// What the `workerd` global asks of the REPL. Outlives every JavaScript object in the REPL's
// isolate.
class ReplHost {
 public:
  virtual ~ReplHost() noexcept(false) = default;

  // Loads the config `source` names and starts serving it. Rejects with a JSG error, listing the
  // config errors, if the config does not load or has errors.
  virtual kj::Promise<kj::Own<ReplServerHost>> loadConfig(ReplConfigSource source) = 0;

  // Starts an environment with compatibility settings of its own. Rejects with a JSG error if the
  // settings are not valid.
  virtual kj::Promise<kj::Own<ReplEnvironmentHost>> newEnvironment(
      ReplEnvironmentSettings settings) = 0;
};

// A config the REPL serves: `await workerd.loadConfig(path)`.
class ReplServer final: public jsg::Object {
 public:
  explicit ReplServer(kj::Own<ReplServerHost> host): host(kj::mv(host)) {}

  struct ServiceOptions {
    jsg::Optional<kj::String> entrypoint;
    jsg::Optional<jsg::JsRef<jsg::JsObject>> props;

    JSG_STRUCT(entrypoint, props);
  };

  kj::StringPtr getPath() {
    return host->getPath();
  }
  bool getClosed() {
    return host->isClosed();
  }
  bool getWatching() {
    return host->isWatching();
  }
  kj::Array<kj::String> getWarnings();

  // A Fetcher for the config's service `name`, for `fetch()` and RPC.
  jsg::Ref<api::Fetcher> service(
      jsg::Lock& js, kj::String name, jsg::Optional<ServiceOptions> options);

  // A Fetcher whose `fetch()` sends the request to the config's socket `name`. Sockets listen on
  // in-process names rather than on their addresses.
  jsg::Ref<api::Fetcher> socket(jsg::Lock& js, kj::String name);

  // Names a Durable Object by one of its name, as `idFromName()` takes it, and its id, as
  // `idFromString()` does.
  struct DurableObjectName {
    jsg::Optional<kj::String> name;
    jsg::Optional<kj::String> id;

    JSG_STRUCT(name, id);
  };
  // A name, or a DurableObjectName. An object is never taken for a name: every object is a
  // DurableObjectName.
  using DurableObjectKey = kj::OneOf<kj::String, DurableObjectName>;

  // A stub, for RPC and `fetch()`, of the Durable Object of class `className` that the config's
  // service `service` exports, named by `key`.
  jsg::Ref<api::Fetcher> durableObject(
      jsg::Lock& js, kj::String service, kj::String className, DurableObjectKey key);

  // Loads the config again in place of what was loaded before; this handle, once it has.
  jsg::Promise<jsg::Ref<ReplServer>> reload(jsg::Lock& js);

  jsg::Promise<void> close(jsg::Lock& js);

  JSG_RESOURCE_TYPE(ReplServer) {
    JSG_READONLY_PROTOTYPE_PROPERTY(path, getPath);
    JSG_READONLY_PROTOTYPE_PROPERTY(closed, getClosed);
    JSG_READONLY_PROTOTYPE_PROPERTY(watching, getWatching);
    JSG_READONLY_PROTOTYPE_PROPERTY(warnings, getWarnings);
    JSG_METHOD(service);
    JSG_METHOD(socket);
    JSG_METHOD(durableObject);
    JSG_METHOD(reload);
    JSG_METHOD(close);
  }

  bool isClosed() const {
    return host->isClosed();
  }

 private:
  kj::Own<ReplServerHost> host;
};

// An environment the REPL started: `await workerd.environment({ compatibilityDate })`.
class ReplEnvironmentHandle final: public jsg::Object {
 public:
  explicit ReplEnvironmentHandle(kj::Own<ReplEnvironmentHost> host): host(kj::mv(host)) {}

  kj::StringPtr getCompatibilityDate() {
    return host->getCompatibilityDate();
  }
  kj::Array<kj::String> getCompatibilityFlags();
  bool getClosed() {
    return host->isClosed();
  }

  // What the REPL would print for `input` typed at it, evaluated in this environment.
  jsg::Promise<kj::String> eval(jsg::Lock& js, kj::String input);

  jsg::Promise<void> close(jsg::Lock& js);

  JSG_RESOURCE_TYPE(ReplEnvironmentHandle) {
    JSG_READONLY_PROTOTYPE_PROPERTY(compatibilityDate, getCompatibilityDate);
    JSG_READONLY_PROTOTYPE_PROPERTY(compatibilityFlags, getCompatibilityFlags);
    JSG_READONLY_PROTOTYPE_PROPERTY(closed, getClosed);
    JSG_METHOD(eval);
    JSG_METHOD(close);
  }

 private:
  kj::Own<ReplEnvironmentHost> host;
};

// The `workerd` global of the REPL.
class ReplControl final: public jsg::Object {
 public:
  ReplControl(ReplHost& host, kj::StringPtr version): host(host), version(version) {}

  struct LoadConfigOptions {
    jsg::Optional<kj::String> constName;
    // Reload the config whenever a file it was loaded from changes.
    jsg::Optional<bool> watch;

    JSG_STRUCT(constName, watch);
  };

  jsg::Promise<jsg::Ref<ReplServer>> loadConfig(
      jsg::Lock& js, kj::String path, jsg::Optional<LoadConfigOptions> options);

  // The servers loaded in this session and not yet closed.
  kj::Array<jsg::Ref<ReplServer>> getServers();

  struct EnvironmentOptions {
    jsg::Optional<kj::String> compatibilityDate;
    jsg::Optional<kj::Array<kj::String>> compatibilityFlags;

    JSG_STRUCT(compatibilityDate, compatibilityFlags);
  };

  // Starts an environment like the REPL's, in an isolate of its own, with the compatibility
  // settings `options` gives rather than the REPL's.
  jsg::Promise<jsg::Ref<ReplEnvironmentHandle>> environment(
      jsg::Lock& js, jsg::Optional<EnvironmentOptions> options);

  // Evicts the Durable Object behind `stub` (from a server's durableObject()) as
  // `cloudflare:unsafe`'s evict() does.
  jsg::Promise<void> evict(jsg::Lock& js,
      jsg::Ref<api::Fetcher> stub,
      jsg::Optional<api::UnsafeModule::EvictOptions> options);

  kj::StringPtr getVersion() {
    return version;
  }

  JSG_RESOURCE_TYPE(ReplControl) {
    JSG_METHOD(loadConfig);
    JSG_METHOD(environment);
    JSG_METHOD(evict);
    JSG_READONLY_PROTOTYPE_PROPERTY(servers, getServers);
    JSG_READONLY_PROTOTYPE_PROPERTY(version, getVersion);
  }

 private:
  ReplHost& host;
  kj::StringPtr version;

  // Every server loadConfig() returned and not yet seen closed. Closed ones are dropped as
  // getServers() finds them.
  kj::Vector<jsg::Ref<ReplServer>> servers;

  void visitForGc(jsg::GcVisitor& visitor) {
    for (auto& server: servers) {
      visitor.visit(server);
    }
  }
};

#define EW_REPL_ISOLATE_TYPES                                                                      \
  workerd::server::ReplControl, workerd::server::ReplControl::LoadConfigOptions,                   \
      workerd::server::ReplServer, workerd::server::ReplServer::ServiceOptions,                    \
      workerd::server::ReplServer::DurableObjectName, workerd::server::ReplEnvironmentHandle,      \
      workerd::server::ReplControl::EnvironmentOptions

}  // namespace workerd::server
