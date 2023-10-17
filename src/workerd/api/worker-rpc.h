// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/http.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/function.h>

namespace workerd::api {

// A WorkerRpc object forwards JS method calls to the remote Worker/Durable Object over RPC.
// Since methods are not known until runtime, WorkerRpc doesn't define any JS methods.
// Instead, we use JSG_NAMED_INTERCEPT to intercept property accesses of names that are not known at
// compile time.
//
// WorkerRpc only supports method calls. You cannot, for instance, access a property of a
// Durable Object over RPC.
class WorkerRpc : public Fetcher, public jsg::NamedIntercept {
public:
  WorkerRpc(
      IoOwn<OutgoingFactory> outgoingFactory,
      RequiresHostAndProtocol requiresHost,
      bool inHouse)
    : Fetcher(kj::mv(outgoingFactory), requiresHost, inHouse) {}

  kj::Maybe<jsg::JsValue> getNamed(jsg::Lock& js, kj::StringPtr name) override;

  // WARNING: Adding a new JSG_METHOD to a class that extends WorkerRpc can conflict with RPC method
  // names defined on your remote target. For example, if you add a new method `bar()` to the
  // Durable Object stub, which extends WorkerRpc, then any scripts with a DO that defines `bar()`
  // and which call `stub.bar()` will stop calling the method over RPC, and start calling the method
  // you're adding to the Durable Object stub.
  //
  // This also applies to classes from which your final stub object is derived from. For example,
  // since the Durable Object stub extends WorkerRpc, and WorkerRpc extends Fetcher, any new
  // JSG_METHOD defined on Fetcher will override name interception on the Durable Object stub.
  //
  // New JSG_METHODs should be gated via compatibility flag/date and should be announced in the
  // change log.
  JSG_RESOURCE_TYPE(WorkerRpc, CompatibilityFlags::Reader flags) {
    if (flags.getWorkerdExperimental()) {
      JSG_NAMED_INTERCEPT();
    }
    JSG_INHERIT(Fetcher);
  }
};

#define EW_WORKER_RPC_ISOLATE_TYPES  \
  api::WorkerRpc

}; // namespace workerd::api
