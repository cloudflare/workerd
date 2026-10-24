#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

// node:diagnostics_channel is implemented in TypeScript (src/node/diagnostics_channel.ts). Only
// forwarding published messages to tail workers lives here, since it needs the IoContext.
class DiagnosticsChannelModule: public jsg::Object {
 public:
  DiagnosticsChannelModule() = default;
  DiagnosticsChannelModule(jsg::Lock&, const jsg::Url&) {}

  // Called by Channel.publish() after the subscribers have run. Records the message for tail
  // workers if the current request has a tracer. Throws if there is no current request.
  void publishToTracer(jsg::Lock& js, jsg::JsValue name, jsg::JsValue message);

  JSG_RESOURCE_TYPE(DiagnosticsChannelModule) {
    JSG_METHOD(publishToTracer);
  }
};

#define EW_NODE_DIAGNOSTICCHANNEL_ISOLATE_TYPES api::node::DiagnosticsChannelModule

}  // namespace workerd::api::node
