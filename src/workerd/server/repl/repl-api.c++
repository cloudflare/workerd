// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "repl-api.h"

#include <workerd/io/io-context.h>

namespace workerd::server {

kj::Array<kj::String> ReplServer::getWarnings() {
  return KJ_MAP(warning, host->getWarnings()) { return kj::str(warning); };
}

jsg::Ref<api::Fetcher> ReplServer::service(
    jsg::Lock& js, kj::String name, jsg::Optional<ServiceOptions> options) {
  kj::Maybe<kj::String> entrypoint;
  kj::Maybe<Frankenvalue> props;
  KJ_IF_SOME(o, options) {
    entrypoint = kj::mv(o.entrypoint);
    KJ_IF_SOME(p, o.props) {
      props = Frankenvalue::fromJs(js, p.getHandle(js));
    }
  }
  auto channel = host->service(name, entrypoint, kj::mv(props));
  return js.alloc<api::Fetcher>(IoContext::current().addObject(kj::mv(channel)));
}

jsg::Ref<api::Fetcher> ReplServer::socket(jsg::Lock& js, kj::String name) {
  return js.alloc<api::Fetcher>(IoContext::current().addObject(host->socket(name)));
}

namespace {
ReplActorAddress actorAddress(
    kj::StringPtr service, kj::StringPtr className, const ReplServer::DurableObjectKey& key) {
  ReplActorAddress address{.service = service, .className = className};
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(name, kj::String) {
      address.name = name;
    }
    KJ_CASE_ONEOF(names, ReplServer::DurableObjectName) {
      address.name = names.name;
      address.id = names.id;
    }
  }
  return address;
}
}  // namespace

jsg::Ref<api::Fetcher> ReplServer::durableObject(
    jsg::Lock& js, kj::String service, kj::String className, DurableObjectKey key) {
  auto channel = host->durableObject(actorAddress(service, className, key));
  return js.alloc<api::Fetcher>(IoContext::current().addObject(kj::mv(channel)));
}

jsg::Promise<jsg::Ref<ReplServer>> ReplServer::reload(jsg::Lock& js) {
  // Like closing, reloading starts and ends isolates, on the event loop.
  return IoContext::current().awaitIo(
      js, host->reload(), [self = JSG_THIS](jsg::Lock&) mutable { return kj::mv(self); });
}

jsg::Promise<void> ReplServer::close(jsg::Lock& js) {
  // Closing runs on the event loop: it ends the server's isolates, which must not be locked while
  // this isolate is.
  return IoContext::current().awaitIo(js, host->close());
}

jsg::Promise<jsg::Ref<ReplServer>> ReplControl::loadConfig(
    jsg::Lock& js, kj::String path, jsg::Optional<LoadConfigOptions> options) {
  ReplConfigSource source{.path = kj::mv(path)};
  KJ_IF_SOME(o, options) {
    source.constName = kj::mv(o.constName);
    source.watch = o.watch.orDefault(false);
  }
  return IoContext::current().awaitIo(js, host.loadConfig(kj::mv(source)),
      [self = JSG_THIS](jsg::Lock& js, kj::Own<ReplServerHost> server) mutable {
    auto handle = js.alloc<ReplServer>(kj::mv(server));
    self->servers.add(handle.addRef());
    return handle;
  });
}

kj::Array<kj::String> ReplEnvironmentHandle::getCompatibilityFlags() {
  return KJ_MAP(flag, host->getCompatibilityFlags()) { return kj::str(flag); };
}

jsg::Promise<kj::String> ReplEnvironmentHandle::eval(jsg::Lock& js, kj::String input) {
  // The input runs under the environment's isolate lock, on the event loop.
  return IoContext::current().awaitIo(js, host->evaluate(kj::mv(input)));
}

jsg::Promise<void> ReplEnvironmentHandle::close(jsg::Lock& js) {
  return IoContext::current().awaitIo(js, host->close());
}

jsg::Promise<jsg::Ref<ReplEnvironmentHandle>> ReplControl::environment(
    jsg::Lock& js, jsg::Optional<EnvironmentOptions> options) {
  ReplEnvironmentSettings settings;
  KJ_IF_SOME(o, options) {
    settings.compatibilityDate = kj::mv(o.compatibilityDate);
    settings.compatibilityFlags = kj::mv(o.compatibilityFlags).orDefault({});
  }
  return IoContext::current().awaitIo(js, host.newEnvironment(kj::mv(settings)),
      [](jsg::Lock& js, kj::Own<ReplEnvironmentHost> environment) {
    return js.alloc<ReplEnvironmentHandle>(kj::mv(environment));
  });
}

jsg::Promise<void> ReplControl::evict(jsg::Lock& js,
    jsg::Ref<api::Fetcher> stub,
    jsg::Optional<api::UnsafeModule::EvictOptions> options) {
  return js.alloc<api::UnsafeModule>()->evict(js, kj::mv(stub), kj::mv(options));
}

kj::Array<jsg::Ref<ReplServer>> ReplControl::getServers() {
  kj::Vector<jsg::Ref<ReplServer>> open(servers.size());
  for (auto& server: servers) {
    if (!server->isClosed()) {
      open.add(server.addRef());
    }
  }
  servers = kj::mv(open);
  return KJ_MAP(server, servers) { return server.addRef(); };
}

}  // namespace workerd::server
