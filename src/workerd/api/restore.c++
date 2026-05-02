// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "restore.h"

#include "http.h"
#include "worker-rpc.h"

namespace workerd::api {

using RestoreResult = kj::OneOf<jsg::Ref<Fetcher>, jsg::Ref<JsRpcStub>>;

// Invoke the entrypoint's `[restore]()` method and coerce the result to either Fetcher or
// JsRpcStub.
static jsg::Promise<RestoreResult> invokeRestoreAndCoerce(
    jsg::Lock& js, jsg::JsObject target, jsg::JsValue params) {
  auto restoreValue = target.get(js, js.symbolInternal("cloudflare:workers:restore"));
  auto restoreFunction = JSG_REQUIRE_NONNULL(restoreValue.tryCast<jsg::JsFunction>(), TypeError,
      "The WorkerEntrypoint or DurableObject class does not implement a [restore]() method.");

  auto result = restoreFunction.call(js, target, params);
  return js.toPromise(result).then(js, [](jsg::Lock& js, jsg::Value value) -> RestoreResult {
    auto jsValue = jsg::JsValue(value.getHandle(js));
    auto object = JSG_REQUIRE_NONNULL(jsValue.tryCast<jsg::JsObject>(), TypeError,
        "The [restore]() method must return a ServiceStub or RpcStub.");

    KJ_IF_SOME(fetcher, object.tryUnwrapAs<Fetcher>(js)) {
      return fetcher.addRef();
    }

    KJ_IF_SOME(stub, object.tryUnwrapAs<JsRpcStub>(js)) {
      return stub.addRef();
    }

    // Types which implicitly convert to RpcStub (RpcTargets and functions) should be accepted
    // as a return value, too.
    if (JsRpcStub::shouldImplicitlyStubify(js, object)) {
      return JsRpcStub::constructor(js, object);
    }

    JSG_FAIL_REQUIRE(TypeError, "The [restore]() method must return a ServiceStub or RpcStub.");
  });
}

jsg::Promise<jsg::Value> restoreCurrentEntrypoint(jsg::Lock& js,
    jsg::JsObject params,
    const jsg::TypeHandler<jsg::Ref<Fetcher>>& fetcherHandler,
    const jsg::TypeHandler<jsg::Ref<JsRpcStub>>& rpcStubHandler) {
  auto restoreParams = Frankenvalue::fromJs(js, jsg::JsValue(params));
  auto target = IoContext::current().getEntrypointHandler(js);

  return invokeRestoreAndCoerce(js, target, params)
      .then(js,
          [&fetcherHandler, &rpcStubHandler, restoreParams = kj::mv(restoreParams)](
              jsg::Lock& js, RestoreResult result) mutable -> jsg::Value {
    auto& ioctx = IoContext::current();
    auto selfTokenFactory = JSG_REQUIRE_NONNULL(ioctx.getSelfTokenFactory(), Error,
        "ctx.restore() cannot be used in this context because the system does not know how to "
        "restore this context itself, in order to be able to invoke its [restore]() method. "
        "This may happen if the current worker is a Dynamic Worker or a Durable Object Facet (or "
        "both), and the immediate caller did not specify how to restore it. To fix this, ensure "
        "that the caller of this Worker uses a stub that was created via its own "
        "context.restore().");

    auto& factory = ioctx.getIoChannelFactory();

    KJ_SWITCH_ONEOF(result) {
      KJ_CASE_ONEOF(fetcher, jsg::Ref<Fetcher>) {
        auto baseChannel = fetcher->getSubrequestChannel(ioctx);
        auto channel = factory.makeRestoredSubrequestChannel(
            kj::addRef(*selfTokenFactory), kj::mv(restoreParams), kj::mv(baseChannel));
        auto restored = js.alloc<Fetcher>(ioctx.addObject(kj::mv(channel)));
        return jsg::Value(js.v8Isolate, fetcherHandler.wrap(js, kj::mv(restored)));
      }
      KJ_CASE_ONEOF(stub, jsg::Ref<JsRpcStub>) {
        auto channel =
            factory.makeRestoredRpcChannel(kj::addRef(*selfTokenFactory), kj::mv(restoreParams));
        auto client = stub->getClient();
        stub->dispose();
        auto restored = js.alloc<JsRpcStub>(
            ioctx.addObject(kj::heap(kj::mv(client))), ioctx.addObject(kj::mv(channel)));
        return jsg::Value(js.v8Isolate, rpcStubHandler.wrap(js, kj::mv(restored)));
      }
    }
    KJ_UNREACHABLE;
  });
}

};  // namespace workerd::api
