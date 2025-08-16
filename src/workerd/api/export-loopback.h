// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "http.h"

namespace workerd::api {

// LoopbackServiceStub is the type of a property of `ctx.exports` which points back at a stateless
// (non-actor) entrypoint of this Worker. It can be used as a regular Fetcher to make calls to that
// entrypoint with empty props. It can also be invoked as a function in order to specialize it with
// props and make it available for RPC.
class LoopbackServiceStub: public Fetcher {
 public:
  // Loopback services are always represented by numbered subrequest channels.
  explicit LoopbackServiceStub(uint channel)
      : Fetcher(channel, RequiresHostAndProtocol::YES, /*isInHouse=*/true),
        channel(channel) {}

  struct Options {
    jsg::Optional<jsg::JsRef<jsg::JsObject>> props;

    JSG_STRUCT(props);
  };

  // Create a specialized Fetcher which can be passed over RPC.
  jsg::Ref<Fetcher> call(jsg::Lock& js, Options options);

  JSG_RESOURCE_TYPE(LoopbackServiceStub) {
    JSG_INHERIT(Fetcher);
    JSG_CALLABLE(call);
  }

 private:
  uint channel;
};

#define EW_EXPORT_LOOPBACK_ISOLATE_TYPES api::LoopbackServiceStub, api::LoopbackServiceStub::Options

}  // namespace workerd::api
