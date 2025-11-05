// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "http.h"
#include "worker-rpc.h"

namespace workerd::api {

// Base class for exported RPC services.
//
// When the worker's top-level module exports a class that extends this class, it means that it
// is a stateless service.
//
//     import {WorkerEntrypoint} from "cloudflare:workers";
//     export class MyService extends WorkerEntrypoint {
//       async fetch(req) { ... }
//       async someRpcMethod(a, b) { ... }
//     }
//
// `env` and `ctx` are automatically available as `this.env` and `this.ctx`, without the need to
// define a constructor.
class WorkerEntrypoint: public jsg::Object {
 public:
  static jsg::Ref<WorkerEntrypoint> constructor(
      const v8::FunctionCallbackInfo<v8::Value>& args, jsg::JsObject ctx, jsg::JsObject env);

  JSG_RESOURCE_TYPE(WorkerEntrypoint) {}
};

// Like WorkerEntrypoint, but this is the base class for Durable Object classes.
//
// Note that the name of this class as seen by JavaScript is `DurableObject`, but using that name
// in C++ would conflict with the type name currently used by DO stubs.
// TODO(cleanup): Rename DO stubs to `DurableObjectStub`?
//
// Historically, DO classes were not expected to inherit anything. However, this made it impossible
// to tell whether an exported class was intended to be a DO class vs. something else. Originally
// there were no other kinds of exported classes so this was fine. Going forward, we encourage
// everyone to be explicit by inheriting this, and we require it if you want to use RPC.
class DurableObjectBase: public jsg::Object {
 public:
  static jsg::Ref<DurableObjectBase> constructor(const v8::FunctionCallbackInfo<v8::Value>& args,
      jsg::Ref<DurableObjectState> ctx,
      jsg::JsObject env);

  JSG_RESOURCE_TYPE(DurableObjectBase) {}
};

// Base class for Workflows
//
// When the worker's top-level module exports a class that extends this class, it means that it
// is a Workflow.
//
//     import { WorkflowEntrypoint } from "cloudflare:workers";
//     export class MyWorkflow extends WorkflowEntrypoint {
//       async run(batch, fns) { ... }
//     }
//
// `env` and `ctx` are automatically available as `this.env` and `this.ctx`, without the need to
// define a constructor.
class WorkflowEntrypoint: public jsg::Object {
 public:
  static jsg::Ref<WorkflowEntrypoint> constructor(const v8::FunctionCallbackInfo<v8::Value>& args,
      jsg::Ref<ExecutionContext> ctx,
      jsg::JsObject env);

  JSG_RESOURCE_TYPE(WorkflowEntrypoint) {}
};

// The "cloudflare:workers" module, which exposes the WorkerEntrypoint, WorkflowEntrypoint and DurableObject types
// for extending.
class EntrypointsModule: public jsg::Object {
 public:
  EntrypointsModule() = default;
  EntrypointsModule(jsg::Lock&, const jsg::Url&) {}

  void waitUntil(kj::Promise<void> promise);

  JSG_RESOURCE_TYPE(EntrypointsModule) {
    JSG_NESTED_TYPE(WorkerEntrypoint);
    JSG_NESTED_TYPE(WorkflowEntrypoint);
    JSG_NESTED_TYPE_NAMED(DurableObjectBase, DurableObject);
    JSG_NESTED_TYPE_NAMED(JsRpcPromise, RpcPromise);
    JSG_NESTED_TYPE_NAMED(JsRpcProperty, RpcProperty);
    JSG_NESTED_TYPE_NAMED(JsRpcStub, RpcStub);
    JSG_NESTED_TYPE_NAMED(JsRpcTarget, RpcTarget);
    JSG_NESTED_TYPE_NAMED(Fetcher, ServiceStub);

    JSG_METHOD(waitUntil);
  }
};

#define EW_WORKERS_MODULE_ISOLATE_TYPES                                                            \
  api::WorkerEntrypoint, api::WorkflowEntrypoint, api::DurableObjectBase, api::EntrypointsModule

template <class Registry>
void registerWorkersModule(Registry& registry, CompatibilityFlags::Reader flags) {
  registry.template addBuiltinModule<EntrypointsModule>(
      "cloudflare-internal:workers", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalRpcModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  static const auto kSpecifier = "cloudflare-internal:workers"_url;
  builder.addObject<EntrypointsModule, TypeWrapper>(kSpecifier);
  return builder.finish();
}
};  // namespace workerd::api
