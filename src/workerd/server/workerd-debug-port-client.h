// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-channels.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {
class Fetcher;
}  // namespace workerd::api

namespace workerd::server {

// JS interface for a workerd debug port binding.
// This binding provides access to a remote workerd instance's WorkerdDebugPort RPC interface,
// allowing dynamic access to worker entrypoints without requiring static configuration.
class WorkerdDebugPortClient: public jsg::Object {
 public:
  // Create a WorkerdDebugPortClient backed by the given channel number.
  explicit WorkerdDebugPortClient(uint channel): channel(channel) {}

  // Get access to a stateless entrypoint on the remote workerd instance.
  //
  // @param service - The service name in the remote workerd process
  // @param entrypoint - The entrypoint name to access (if omitted, uses the default handler)
  // @param props - Optional props to pass to the entrypoint
  // @returns A Promise<Fetcher> that can be used to invoke the entrypoint
  jsg::Promise<jsg::Ref<api::Fetcher>> getEntrypoint(jsg::Lock& js,
      kj::String service,
      jsg::Optional<kj::String> entrypoint,
      jsg::Optional<jsg::JsRef<jsg::JsObject>> props);

  // Get access to an actor (Durable Object) stub on the remote workerd instance.
  //
  // @param service - The service name in the remote workerd process
  // @param entrypoint - The entrypoint/class name to access
  // @param actorId - The actor ID (hex string for DOs, plain string for ephemeral)
  // @returns A Promise<Fetcher> that can be used to invoke the actor
  jsg::Promise<jsg::Ref<api::Fetcher>> getActor(
      jsg::Lock& js, kj::String service, kj::String entrypoint, kj::String actorId);

  JSG_RESOURCE_TYPE(WorkerdDebugPortClient) {
    JSG_METHOD(getEntrypoint);
    JSG_METHOD(getActor);

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE({
      getEntrypoint<T extends Rpc.WorkerEntrypointBranded | undefined>(
          service: string, entrypoint?: string, props?: Record<string, unknown>): Promise<Fetcher<T>>;
      getActor<T extends Rpc.DurableObjectBranded | undefined>(
          service: string, entrypoint: string, actorId: string): Promise<Fetcher<T>>;
    });
  }

 private:
  uint channel;
};

#define EW_WORKERD_DEBUG_PORT_CLIENT_ISOLATE_TYPES workerd::server::WorkerdDebugPortClient

}  // namespace workerd::server
