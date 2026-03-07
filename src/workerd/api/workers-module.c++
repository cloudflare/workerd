// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "workers-module.h"

#include <workerd/api/actor-state.h>
#include <workerd/api/global-scope.h>
#include <workerd/io/tracer.h>
#include <workerd/io/worker.h>

#include <kj/refcount.h>

namespace workerd::api {
namespace {

// Creates a JS function that wraps `callback` with a "workflow.step.do" tracing span.
// When the engine calls this wrapper back (via RPC re-entrance), it:
//   1. Opens a TraceContext span tagged with the step name
//   2. Stores the SpanId in AsyncContextFrame for log attribution
//   3. Calls the original local callback
//   4. Ties the span lifetime to the returned promise
v8::Local<v8::Function> makeTracingCallbackWrapper(
    jsg::Lock& js, kj::String stepName, jsg::V8Ref<v8::Value> cbRef) {
  return js.wrapPromiseReturningFunction(js.v8Context(),
      JSG_VISITABLE_LAMBDA((stepName = kj::mv(stepName), cbRef = kj::mv(cbRef)), (cbRef),
          // clang-format off
          (jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& cbInfo)
              -> jsg::Promise<jsg::Value> {
            JSG_TRY(js) {
              jsg::Value stepContext = js.v8Ref(cbInfo[0]);
              kj::String dedupName = js.toString(cbInfo[1]);

              kj::Maybe<kj::Own<kj::RefcountedWrapper<kj::Own<TraceContext>>>> traceContextHolder;
              kj::Maybe<jsg::AsyncContextFrame::StorageScope> maybeStorageScope;
              KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
                auto traceContext =
                    kj::heap<TraceContext>(ioContext.makeUserTraceSpan("workflow.step.do"_kjc));
                traceContext->setTag("workflow.step.name"_kjc, kj::mv(stepName));
                traceContext->setTag("workflow.step.unique_name"_kjc, kj::mv(dedupName));

                // Store the span's SpanId in AsyncContextFrame so handleLog
                // can attribute console.log calls to this step span.
                KJ_IF_SOME(stepSpanId, traceContext->getUserSpanId()) {
                  auto bigint = v8::BigInt::NewFromUnsigned(js.v8Isolate, stepSpanId.getId());
                  auto& key = ioContext.getWorker().getWorkflowStepSpanKey();
                  maybeStorageScope.emplace(js, key, js.v8Ref(bigint.As<v8::Value>()));
                }

                traceContextHolder = kj::refcountedWrapper(kj::mv(traceContext));
              }

              auto callback = jsg::JsFunction(cbRef.getHandle(js).As<v8::Function>());
              auto result = callback.callNoReceiver(js, jsg::JsValue(stepContext.getHandle(js)));
              auto promise = js.toPromise(v8::Local<v8::Value>(result));

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
                      jsg::Lock& js, jsg::Value value) mutable -> jsg::Value { return kj::mv(value); },
                  [traceContext = kj::mv(rejectRef)](jsg::Lock& js,
                      jsg::Value error) mutable -> jsg::Value { js.throwException(kj::mv(error)); });
            }
            JSG_CATCH(exception) {
              return js.rejectedPromise<jsg::Value>(kj::mv(exception));
            }
          }
          // clang-format on
          ));
}  // namespace workerd::api

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
    jsg::Lock& js, jsg::Value event, jsg::Value step) {
  auto stepObj = JSG_REQUIRE_NONNULL(jsg::JsValue(step.getHandle(js)).tryCast<jsg::JsObject>(),
      TypeError, "step must be an object");
  auto originalDo = stepObj.get(js, "do"_kj);
  JSG_REQUIRE(originalDo.isFunction(), TypeError, "step object missing 'do' method");

  auto originalDoRef = js.v8Ref(v8::Local<v8::Value>(originalDo));
  auto stepRef = js.v8Ref(v8::Local<v8::Value>(v8::Local<v8::Object>(stepObj)));

  auto patchedDo = js.wrapPromiseReturningFunction(js.v8Context(),
      JSG_VISITABLE_LAMBDA((originalDoRef = kj::mv(originalDoRef), stepRef = kj::mv(stepRef)),
          (originalDoRef, stepRef),
          // clang-format off
          (jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info)
              -> jsg::Promise<jsg::Value> {
            JSG_TRY(js) {
              v8::LocalVector<v8::Value> argv(js.v8Isolate);
              for (int i = 0; i < info.Length(); ++i) {
                argv.push_back(info[i]);
              }

              for (int i = argv.size() - 1; i >= 0; --i) {
                if (!argv[i]->IsFunction()) continue;
                auto stepName = js.toString(argv[0]);
                auto cbRef = js.v8Ref(argv[i]);
                argv[i] = makeTracingCallbackWrapper(js, kj::mv(stepName), kj::mv(cbRef));
                break;
              }

              auto origFunc = jsg::JsFunction(originalDoRef.getHandle(js).As<v8::Function>());
              auto stepTarget = jsg::JsObject(stepRef.getHandle(js).As<v8::Object>());
              return js.toPromise(v8::Local<v8::Value>(origFunc.call(js, stepTarget, argv)));
            }
            JSG_CATCH(exception) {
              return js.rejectedPromise<jsg::Value>(kj::mv(exception));
            }
          }
          // clang-format on
          ));

stepObj.set(js, "do"_kj, jsg::JsValue(patchedDo));

auto selfRef = JSG_THIS;
auto selfHandle = JSG_REQUIRE_NONNULL(
    selfRef.tryGetHandle(js), Error, "WorkflowEntrypoint JS wrapper not initialized");
jsg::JsObject self(selfHandle);
auto runFunc = JSG_REQUIRE_NONNULL(self.get(js, "run"_kj).tryCast<jsg::JsFunction>(),
    TypeError,
    "WorkflowEntrypoint subclass must define a run() method");

auto result =
    runFunc.call(js, self, jsg::JsValue(event.getHandle(js)), jsg::JsValue(step.getHandle(js)));

return js.toPromise(v8::Local<v8::Value>(result));
}

void EntrypointsModule::waitUntil(kj::Promise<void> promise) {
  // No need to check if IoContext::hasCurrent since current() will throw
  // if there is no active request.
  IoContext::current().addWaitUntil(kj::mv(promise));
}

}  // namespace workerd::api
