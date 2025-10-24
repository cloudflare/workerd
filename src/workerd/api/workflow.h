// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/worker-rpc.h>
#include <workerd/api/basics.h>
#include <workerd/io/trace.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/io/worker-interface.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/jsvalue.h>

#include <kj/async.h>
#include <kj/common.h>

namespace workerd::api {

class ExecutionContext;

// Event handler types

// Types for other workers passing messages into and responses out of a queue handler.

// What the Workflows engine passes the userland worker
struct IncomingWorkflowInvocation {
  explicit IncomingWorkflowInvocation(
      kj::String workflowName, kj::String instanceId, kj::Date timestamp, jsg::Value payload)
      : workflowName(kj::mv(workflowName)),
        instanceId(kj::mv(instanceId)),
        timestamp(kj::mv(timestamp)),
        payload(kj::mv(payload)) {}

  kj::String workflowName;
  kj::String instanceId;
  kj::Date timestamp;
  jsg::Value payload;
  JSG_STRUCT(workflowName, instanceId, timestamp, payload);
};

// NOTE(lduarte): for backwards compat, we didn't properly validate if the defined `class_name` was
// a WorkflowEntrypoint - it means that we have to accept _all_ entrypoint types that have a `run` method.
// TODO(lduarte): can I use the validation compat flag to restrict this in newer workers?
struct WorkflowRunHandler {
  using RunHandler = jsg::Promise<jsg::JsRef<jsg::JsValue>>(
      IncomingWorkflowInvocation event, jsg::Ref<JsRpcStub> step);
  jsg::LenientOptional<jsg::Function<RunHandler>> run;

  JSG_STRUCT(run);
};

struct WorkflowInvocationResult {
  struct Serialized {
    kj::Maybe<kj::Array<kj::byte>> own;
    // Holds onto the owner of a given array of serialized data.
    kj::ArrayPtr<kj::byte> data;
    // A pointer into that data that can be directly written into it, regardless
    // of its holder.
  };

  jsg::JsRef<jsg::JsValue> returnValue;

  JSG_STRUCT(returnValue);
};

class WorkflowCustomEventImpl final: public WorkerInterface::CustomEvent, public kj::Refcounted {
 public:
  WorkflowCustomEventImpl(kj::OneOf<IncomingWorkflowInvocation,
                              rpc::EventDispatcher::RunWorkflowInvocationParams::Reader> params,
      kj::Own<rpc::JsRpcTarget::Client> stepStub)
      : params(kj::mv(params)),
        stepStub(kj::mv(stepStub)) {}

  kj::Promise<Result> run(kj::Own<IoContext_IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName,
      Frankenvalue props,
      kj::TaskSet& waitUntilTasks) override;

  kj::Promise<Result> sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      rpc::EventDispatcher::Client dispatcher) override;

  static const uint16_t EVENT_TYPE = 5;
  uint16_t getType() override {
    return EVENT_TYPE;
  }

  kj::Maybe<tracing::EventInfo> getEventInfo() const override;

  WorkflowInvocationResult getInvocationResult(jsg::Lock& js);

  void failed(const kj::Exception& e) override {};

  kj::Promise<Result> notSupported() override {
    return Result{EventOutcome::UNKNOWN};
  }

 private:
  kj::OneOf<IncomingWorkflowInvocation, rpc::EventDispatcher::RunWorkflowInvocationParams::Reader>
      params;
  kj::Own<rpc::JsRpcTarget::Client> stepStub;
  kj::Maybe<WorkflowInvocationResult::Serialized> result;
};

#define EW_WORKFLOW_ISOLATE_TYPES                                                                  \
  api::IncomingWorkflowInvocation, api::WorkflowRunHandler, api::WorkflowInvocationResult
}  // namespace workerd::api
