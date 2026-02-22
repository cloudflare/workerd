// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "workers-module.h"

#include <kj/refcount.h>
#include <workerd/api/actor-state.h>
#include <workerd/api/global-scope.h>
#include <workerd/io/tracer.h>
#include <workerd/io/worker.h>

namespace workerd::api {
namespace {

// Creates a JS function that wraps `callback` with a "workflow.step.do" tracing span.
// When the engine calls this wrapper back (via RPC re-entrance), it:
//   1. Opens a TraceContext span tagged with the step name
//   2. Stores the SpanId in AsyncContextFrame for log attribution
//   3. Calls the original local callback
//   4. Ties the span lifetime to the returned promise
v8::Local<v8::Function> makeTracingCallbackWrapper(
    jsg::Lock& js,
    jsg::V8Ref<v8::Value> stepNameRef,
    jsg::V8Ref<v8::Value> cbRef) {
  return js.wrapReturningFunction(js.v8Context(),
      JSG_VISITABLE_LAMBDA(
          (stepNameRef = kj::mv(stepNameRef),
           cbRef = kj::mv(cbRef)),
          (stepNameRef, cbRef),
          (jsg::Lock& js,
              const v8::FunctionCallbackInfo<v8::Value>& cbInfo)
                  -> v8::Local<v8::Value> {
        // Create the tracing span.
        kj::Maybe<kj::Own<kj::RefcountedWrapper<kj::Own<TraceContext>>>>
            traceContextHolder;
        kj::Maybe<jsg::AsyncContextFrame::StorageScope> maybeStorageScope;
        KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
          auto stepName = js.toString(
              v8::Local<v8::Value>(stepNameRef.getHandle(js)));
          auto traceContext = kj::heap<TraceContext>(
              ioContext.makeUserTraceSpan("workflow.step.do"_kjc));
          traceContext->setTag("workflow.step.name"_kjc, kj::mv(stepName));

          // The engine passes a deduplicated step name as cbInfo[1].
          if (cbInfo.Length() >= 2 && cbInfo[1]->IsString()) {
            auto dedupName = js.toString(cbInfo[1]);
            traceContext->setTag(
                "workflow.step.unique_name"_kjc, kj::mv(dedupName));
          }

          // Store the span's SpanId in AsyncContextFrame so handleLog
          // can attribute console.log calls to this step span.
          KJ_IF_SOME(stepSpanId, traceContext->getUserSpanId()) {
            auto bigint = v8::BigInt::NewFromUnsigned(
                js.v8Isolate, stepSpanId.getId());
            auto& key = ioContext.getWorker().getWorkflowStepSpanKey();
            maybeStorageScope.emplace(
                js, key, js.v8Ref(bigint.As<v8::Value>()));
          }

          traceContextHolder =
              kj::refcountedWrapper(kj::mv(traceContext));
        }

        // Call the original local callback with stepContext (cbInfo[0]).
        // The deduplicated step name (cbInfo[1]) is consumed above for
        // span tagging and is not forwarded to user code.
        auto callback = cbRef.getHandle(js).As<v8::Function>();
        v8::LocalVector<v8::Value> callbackArgs(js.v8Isolate);
        if (cbInfo.Length() >= 1) {
          callbackArgs.push_back(cbInfo[0]);
        }
        auto result = jsg::check(callback->Call(
            js.v8Context(), js.v8Undefined(),
            callbackArgs.size(), callbackArgs.data()));

        auto promise = js.toPromise(result);

        // Tie span lifetime to the promise — the TraceContext is destroyed
        // when the promise settles, closing the span deterministically.
        kj::Maybe<kj::Own<TraceContext>> fulfillRef;
        kj::Maybe<kj::Own<TraceContext>> rejectRef;
        KJ_IF_SOME(holder, traceContextHolder) {
          fulfillRef.emplace(holder->addWrappedRef());
          rejectRef.emplace(holder->addWrappedRef());
        }

        return promise.then(js,
            [traceContext = kj::mv(fulfillRef)](
                jsg::Lock& js, jsg::Value value) mutable -> jsg::Value {
              traceContext = kj::none;
              return kj::mv(value);
            },
            [traceContext = kj::mv(rejectRef)](
                jsg::Lock& js, jsg::Value error) mutable -> jsg::Value {
              traceContext = kj::none;
              js.throwException(kj::mv(error));
            }).consumeHandle(js);
      }));
}

}  // namespace

jsg::Ref<WorkerEntrypoint> WorkerEntrypoint::constructor(
    const v8::FunctionCallbackInfo<v8::Value>& args, jsg::JsObject ctx, jsg::JsObject env) {
  // HACK: We take `FunctionCallbackInfo` mostly so that we can set properties directly on
  //   `This()`. There ought to be a better way to get access to `this` in a constructor.
  //   We *also* declare `ctx` and `env` params more explicitly just for the sake of type checking.
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  jsg::JsObject self(args.This());
  self.set(js, "ctx", jsg::JsValue(args[0]));
  self.set(js, "env", jsg::JsValue(args[1]));
  return js.alloc<WorkerEntrypoint>();
}

jsg::Ref<DurableObjectBase> DurableObjectBase::constructor(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    jsg::Ref<DurableObjectState> ctx,
    jsg::JsObject env) {
  // HACK: We take `FunctionCallbackInfo` mostly so that we can set properties directly on
  //   `This()`. There ought to be a better way to get access to `this` in a constructor.
  //   We *also* declare `ctx` and `env` params more explicitly just for the sake of type checking.
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  jsg::JsObject self(args.This());
  self.set(js, "ctx", jsg::JsValue(args[0]));
  self.set(js, "env", jsg::JsValue(args[1]));
  return js.alloc<DurableObjectBase>();
}

jsg::Ref<WorkflowEntrypoint> WorkflowEntrypoint::constructor(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    jsg::Ref<ExecutionContext> ctx,
    jsg::JsObject env) {
  // HACK: We take `FunctionCallbackInfo` mostly so that we can set properties directly on
  //   `This()`. There ought to be a better way to get access to `this` in a constructor.
  //   We *also* declare `ctx` and `env` params more explicitly just for the sake of type checking.
  jsg::Lock& js = jsg::Lock::from(args.GetIsolate());

  jsg::JsObject self(args.This());
  self.set(js, "ctx", jsg::JsValue(args[0]));
  self.set(js, "env", jsg::JsValue(args[1]));
  return js.alloc<WorkflowEntrypoint>();
}

jsg::Promise<jsg::Value> WorkflowEntrypoint::runStep(
    jsg::Lock& js,
    jsg::Value event,
    jsg::Value step) {
  // Extract workflow metadata from the event object and emit as streaming tail attributes.
  // The Onset event has already been emitted by the time JS code runs, so we emit these as
  // a standalone Attribute event (same pattern as setJsRpcInfo for the method name).
  KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
    KJ_IF_SOME(tracer, ioContext.getWorkerTracer()) {
      auto eventHandle = event.getHandle(js);
      KJ_IF_SOME(eventObj, jsg::JsValue(eventHandle).tryCast<jsg::JsObject>()) {
        auto instanceIdVal = eventObj.get(js, "instanceId"_kj);
        auto workflowNameVal = eventObj.get(js, "workflowName"_kj);
        if (!instanceIdVal.isUndefined() && !workflowNameVal.isUndefined()) {
          tracer.setWorkflowInfo(ioContext.getInvocationSpanContext(), ioContext.now(),
              js.toString(v8::Local<v8::Value>(instanceIdVal)),
              js.toString(v8::Local<v8::Value>(workflowNameVal)));
        }
      }
    }
  }

  auto stepObj = KJ_ASSERT_NONNULL(jsg::JsValue(step.getHandle(js)).tryCast<jsg::JsObject>());
  auto originalDo = stepObj.get(js, "do"_kj);
  KJ_ASSERT(originalDo.isFunction(), "step object missing 'do' method");

  // Capture references for the patched step.do closure.
  auto selfRef = JSG_THIS;
  auto selfHandle = KJ_ASSERT_NONNULL(selfRef.tryGetHandle(js),
      "WorkflowEntrypoint JS wrapper not initialized");
  auto originalDoRef = js.v8Ref(v8::Local<v8::Value>(originalDo));
  auto stepRef = js.v8Ref(v8::Local<v8::Value>(v8::Local<v8::Object>(stepObj)));

  auto patchedDo = js.wrapReturningFunction(js.v8Context(),
      JSG_VISITABLE_LAMBDA(
          (originalDoRef = kj::mv(originalDoRef),
           stepRef = kj::mv(stepRef)),
          (originalDoRef, stepRef),
          (jsg::Lock& js,
              const v8::FunctionCallbackInfo<v8::Value>& info) -> v8::Local<v8::Value> {
        // Build the argv: copy all args from the original step.do call.
        v8::LocalVector<v8::Value> argv(js.v8Isolate);
        for (int i = 0; i < info.Length(); ++i) {
          argv.push_back(info[i]);
        }

        // Find the callback (last function argument) and replace it with a tracing
        // wrapper. When the engine calls this wrapper back via RPC re-entrance, it
        // creates a "workflow.step.do" span around the original local callback.
        for (int i = argv.size() - 1; i >= 0; --i) {
          if (argv[i]->IsFunction()) {
            auto stepNameRef = js.v8Ref(v8::Local<v8::Value>(
                js.str(js.toString(argv[0]))));
            auto cbRef = js.v8Ref(argv[i]);
            argv[i] = makeTracingCallbackWrapper(
                js, kj::mv(stepNameRef), kj::mv(cbRef));
            break;
          }
        }

        // Call the original step.do with the patched argv.
        auto origFunc = originalDoRef.getHandle(js).As<v8::Function>();
        auto stepTarget = stepRef.getHandle(js).As<v8::Object>();
        return jsg::check(
            origFunc->Call(js.v8Context(), stepTarget, argv.size(), argv.data()));
      }));

  // Set the patched do as an own property on the step object.
  // Own properties shadow JSG_WILDCARD_PROPERTY interceptors (kNonMasking flag).
  stepObj.set(js, "do"_kj, jsg::JsValue(patchedDo));

  // Call this.run(event, step) with the now-patched step object.
  jsg::JsObject self(selfHandle);
  auto runMethod = self.get(js, "run"_kj);
  KJ_ASSERT(runMethod.isFunction(), "WorkflowEntrypoint subclass must define a run() method");

  auto runFunc = v8::Local<v8::Value>(runMethod).As<v8::Function>();
  v8::Local<v8::Value> argv[] = {event.getHandle(js), step.getHandle(js)};
  auto result = jsg::check(runFunc->Call(js.v8Context(), self, 2, argv));

  return js.toPromise(result);
}

void EntrypointsModule::waitUntil(kj::Promise<void> promise) {
  // No need to check if IoContext::hasCurrent since current() will throw
  // if there is no active request.
  IoContext::current().addWaitUntil(kj::mv(promise));
}

}  // namespace workerd::api
