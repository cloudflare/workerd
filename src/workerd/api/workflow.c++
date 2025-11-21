
#include "kj/common.h"
#include "kj/debug.h"
#include "kj/memory.h"
#include "kj/one-of.h"
#include "kj/string.h"
#include "kj/time.h"
#include "workerd/io/trace.h"
#include "workflow.h"

#include <workerd/api/global-scope.h>
#include <workerd/io/features.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>

namespace workerd::api {
kj::Maybe<tracing::EventInfo> WorkflowCustomEventImpl::getEventInfo() const {
  kj::String workflowName;
  kj::String instanceId;
  KJ_SWITCH_ONEOF(params) {
    KJ_CASE_ONEOF(p, rpc::EventDispatcher::RunWorkflowInvocationParams::Reader) {
      auto event = p.getEvent();
      workflowName = kj::heapString(event.getWorkflowName());
      instanceId = kj::heapString(event.getInstanceId());
    }
    KJ_CASE_ONEOF(p, IncomingWorkflowInvocation::Serialized) {
      workflowName = kj::heapString(p.workflowName);
      instanceId = kj::heapString(p.instanceId);
    }
  }

  return tracing::EventInfo(tracing::WorkflowEventInfo(kj::mv(workflowName), kj::mv(instanceId)));

}

WorkflowInvocationResult::Serialized serializeResultV8(
    jsg::Lock& js, const jsg::JsRef<jsg::JsValue>& body) {
  // Use a specific serialization version to avoid sending messages using a new version before all
  // runtimes at the edge know how to read it.
  jsg::Serializer serializer(js,
      jsg::Serializer::Options{
        .version = 15,
        .omitHeader = false,
      });

  serializer.write(js, jsg::JsValue(body.getHandle(js)));
  kj::Array<kj::byte> bytes = serializer.release().data;
  WorkflowInvocationResult::Serialized result;
  result.data = bytes;
  result.own = kj::mv(bytes);
  return kj::mv(result);
}

IncomingWorkflowInvocation::Serialized IncomingWorkflowInvocation::Serialized::serializeEventV8(
    jsg::Lock& js, IncomingWorkflowInvocation event) {
  // Use a specific serialization version to avoid sending messages using a new version before all
  // runtimes at the edge know how to read it.
  jsg::Serializer serializer(js,
      jsg::Serializer::Options{
        .version = 15,
        .omitHeader = false,
      });

  serializer.write(js, jsg::JsValue(event.payload.getHandle(js)));
  kj::Array<kj::byte> bytes = serializer.release().data;
  IncomingWorkflowInvocation::Serialized result = IncomingWorkflowInvocation::Serialized(
        kj::mv(event.workflowName), kj::mv(event.instanceId), kj::mv(event.timestamp), kj::mv(bytes));
  return kj::mv(result);
}


WorkflowInvocationResult deserializeResult(
    jsg::Lock& js, const WorkflowInvocationResult::Serialized& body) {
  return WorkflowInvocationResult{
    .returnValue = jsg::JsRef(js, jsg::Deserializer(js, body.data).readValue(js))};
}

jsg::Value getPayloadOrDefault(jsg::Lock& js, IncomingWorkflowInvocation::Serialized& event) {
    KJ_IF_SOME(payloadPtr, event.payloadOwn) {
        return jsg::JsRef(js, jsg::Deserializer(js, payloadPtr.asPtr()).readValue(js));
    } else {
        return js.v8Ref(js.v8Undefined());
    }
}

WorkflowInvocationResult WorkflowCustomEventImpl::getInvocationResult(jsg::Lock& js) {
  KJ_IF_SOME(result, this->result) {
    return deserializeResult(js, result);
  } else {
    JSG_FAIL_REQUIRE(Error, "Workflow invocation didn't return any results.");
  }
}

kj::Promise<WorkerInterface::CustomEvent::Result> WorkflowCustomEventImpl::run(
    kj::Own<IoContext_IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName,
    Frankenvalue props,
    kj::TaskSet& waitUntilTasks) {

  // This method has three main chunks of logic:
  // 1. Do all necessary setup work. This starts right below this comment.
  // 2. Call into the worker's queue event handler.
  // 3. Wait on the necessary portions of the worker's code to complete.
  incomingRequest->delivered();
  auto& context = incomingRequest->getContext();

  // 2. This is where we call into the worker's workflow run function
  auto runProm = context.run(
      [this, entrypointName, &context, props = kj::mv(props)](Worker::Lock& lock) mutable {
    auto& typeHandler = lock.getWorker().getIsolate().getApi().getWorkflowTypeHandler(lock);
    auto maybeExportedHandler =
        lock.getExportedHandler(entrypointName, kj::mv(props), context.getActor());

    KJ_IF_SOME(exportedHandler, maybeExportedHandler) {
      auto workflowHandler =
          KJ_ASSERT_NONNULL(typeHandler.tryUnwrap(lock, exportedHandler->self.getHandle(lock)));

      KJ_IF_SOME(runFunc, workflowHandler.run) {
        // now we build a IncomingWorkflowInvocation event from params in `WorkflowCustomEventImpl`
        jsg::Lock& js = lock;

        KJ_SWITCH_ONEOF(this->params) {
            KJ_CASE_ONEOF(capnpWorkflow, rpc::EventDispatcher::RunWorkflowInvocationParams::Reader) {
                auto capnpEvent = capnpWorkflow.getEvent();
                // TODO(cleanup): this creates a copy of stuff
                auto workflowName = kj::heapString(capnpEvent.getWorkflowName());
                auto instanceId = kj::heapString(capnpEvent.getInstanceId());
                auto timestamp = kj::UNIX_EPOCH + capnpEvent.getTimestampMs() * kj::MILLISECONDS;
                auto payload = jsg::JsRef(js, jsg::Deserializer(js, capnpEvent.getPayload()).readValue(js));
                paramsPtr = kj::heap<IncomingWorkflowInvocation>(
                    kj::mv(workflowName), kj::mv(instanceId), kj::mv(timestamp), kj::mv(payload));
            }
            KJ_CASE_ONEOF(event, IncomingWorkflowInvocation::Serialized) {
                auto payload = getPayloadOrDefault(js, event);
                paramsPtr = kj::heap<IncomingWorkflowInvocation>(
                    kj::mv(event.workflowName), kj::mv(event.instanceId), kj::mv(event.timestamp), kj::mv(payload));
            }
        }



        auto stepStub = js.alloc<JsRpcStub>(context.addObject(kj::mv(this->stepStub)));

        KJ_IF_SOME(tracer, context.getWorkerTracer()) {
            tracer.setWorkflowExecutionModel();
        }

        return context.awaitJs(js,
            runFunc(lock, kj::mv(*paramsPtr), stepStub.addRef())
                .then(js,
                    context.addFunctor(
                        [this](jsg::Lock& js, jsg::JsRef<jsg::JsValue> value) mutable {
          this->result = kj::some(serializeResultV8(js, kj::mv(value)));
        })));
      } else {
        KJ_FAIL_REQUIRE("jsg.Error: run() method does not exist in given entrypoint");
      }

    } else {
      // propagate the call to the caller
      KJ_FAIL_REQUIRE("jsg.Error: given entrypoint passed into the user worker doesn't exist.");
    }

    KJ_UNREACHABLE;
  });

  // Start invoking the `run` handler on the workflow. The promise chain here is intended to mimic the behavior of
  // finishScheduled, but only waiting on the promise returned by the event handler rather than on
  // all waitUntil'ed promises.
  auto outcome =
      co_await runProm.then([]() -> kj::Promise<EventOutcome> { return EventOutcome::OK; })
          .catch_([](kj::Exception&& e) {
    // If any exceptions were thrown, mark the outcome accordingly.
    return EventOutcome::EXCEPTION;
  }).exclusiveJoin(context.onAbort().then([] {
    // Also handle anything that might cause the worker to get aborted.
    return EventOutcome::EXCEPTION;
  }, [](kj::Exception&& e) { return EventOutcome::EXCEPTION; }));

  // TODO: this doesn't wait for waitUntil - I still have to see the correct behavior.
  waitUntilTasks.add(incomingRequest->drain().attach(kj::mv(incomingRequest),
      // kj::addRef(*queueEventHolder),
      kj::addRef(*this)));

  KJ_IF_SOME(status, context.getLimitEnforcer().getLimitsExceeded()) {
    outcome = status;
  }
  co_return WorkerInterface::CustomEvent::Result{.outcome = outcome};
}

kj::Promise<WorkerInterface::CustomEvent::Result> WorkflowCustomEventImpl::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    rpc::EventDispatcher::Client dispatcher) {
  auto req = dispatcher.castAs<rpc::EventDispatcher>().runWorkflowInvocationRequest();
  KJ_UNIMPLEMENTED("sendRpc kaboom!");
  // KJ_SWITCH_ONEOF(params) {
  //   KJ_CASE_ONEOF(p, rpc::EventDispatcher::QueueParams::Reader) {
  //     req.setQueueName(p.getQueueName());
  //     req.setMessages(p.getMessages());
  //   }
  //   KJ_CASE_ONEOF(p, QueueEvent::Params) {
  //     req.setQueueName(p.queueName);
  //     auto messages = req.initMessages(p.messages.size());
  //     for (auto i: kj::indices(p.messages)) {
  //       messages[i].setId(p.messages[i].id);
  //       messages[i].setTimestampNs((p.messages[i].timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
  //       messages[i].setData(p.messages[i].body);
  //       KJ_IF_SOME(contentType, p.messages[i].contentType) {
  //         messages[i].setContentType(contentType);
  //       }
  //       messages[i].setAttempts(p.messages[i].attempts);
  //     }
  //   }
  // }

  // return req.send().then([](auto resp) {
  //   auto respResult = resp.getResult();

  //   return WorkerInterface::CustomEvent::Result{
  //     .outcome = respResult.getOutcome(),
  //   };
  // });
}
}  // namespace workerd::api
