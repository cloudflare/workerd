// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/blob.h>
#include <workerd/api/js-readable-stream.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/jsg/jsg.h>

namespace kj {
class HttpClient;
}

namespace workerd::api {

class ReadableStreamSource;

// Owns a pipelined JSRPC target capability without retaining the JavaScript RpcStub that would
// normally expose it. This transport is local to R2 bindings because their public resource types
// must rebuild gateway results rather than return raw JSRPC values.
class R2RpcClient {
 public:
  explicit R2RpcClient(rpc::JsRpcTarget::Client client);
  R2RpcClient(R2RpcClient&&) = default;
  R2RpcClient& operator=(R2RpcClient&&) = default;
  KJ_DISALLOW_COPY(R2RpcClient);

  static R2RpcClient fromCallResult(jsg::Lock& js, jsg::Value& rpcPromise);

  template <typename... Args>
  jsg::Value call(jsg::Lock& js,
      kj::StringPtr methodName,
      const jsg::TypeHandler<jsg::Ref<JsRpcProperty>>& rpcPropHandler,
      const jsg::TypeHandler<jsg::Function<jsg::Value(Args...)>>& fnHandler,
      Args... args) {
    auto method = getMethod(js, methodName);
    auto disposeStub = kj::defer([&method]() { method.stub->dispose(); });
    auto wrappedProp = rpcPropHandler.wrap(js, kj::mv(method.property));
    auto fn = KJ_ASSERT_NONNULL(fnHandler.tryUnwrap(js, wrappedProp));
    return fn(js, kj::mv(args)...);
  }

 private:
  struct Method {
    jsg::Ref<JsRpcStub> stub;
    jsg::Ref<JsRpcProperty> property;
  };

  Method getMethod(jsg::Lock& js, kj::StringPtr methodName);

  IoOwn<rpc::JsRpcTarget::Client> client;
};

// JsRpcPromise is a custom thenable. Resolving a fresh promise with it makes V8 adopt it even when
// the unwrap_custom_thenables compatibility flag is disabled.
jsg::Promise<jsg::Value> normalizeR2RpcPromise(jsg::Lock& js, jsg::Value rpcPromise);

template <typename... Args>
jsg::Value callR2RpcMethod(jsg::Lock& js,
    jsg::Ref<JsRpcProperty> rpcProp,
    const jsg::TypeHandler<jsg::Ref<JsRpcProperty>>& rpcPropHandler,
    const jsg::TypeHandler<jsg::Function<jsg::Value(Args...)>>& fnHandler,
    Args... args) {
  auto wrappedProp = rpcPropHandler.wrap(js, kj::mv(rpcProp));
  auto fn = KJ_ASSERT_NONNULL(fnHandler.tryUnwrap(js, wrappedProp));
  return fn(js, kj::mv(args)...);
}

template <typename Result>
jsg::Promise<Result> unwrapR2RpcPromise(jsg::Lock& js,
    jsg::Value rpcPromise,
    const jsg::TypeHandler<jsg::Promise<Result>>& resultPromiseHandler) {
  auto normalizedPromise = normalizeR2RpcPromise(js, kj::mv(rpcPromise));
  return KJ_ASSERT_NONNULL(resultPromiseHandler.tryUnwrap(js, normalizedPromise.consumeHandle(js)));
}

template <typename Result, typename... Args>
jsg::Promise<Result> callR2RpcMethod(jsg::Lock& js,
    jsg::Ref<JsRpcProperty> rpcProp,
    const jsg::TypeHandler<jsg::Ref<JsRpcProperty>>& rpcPropHandler,
    const jsg::TypeHandler<jsg::Function<jsg::Value(Args...)>>& fnHandler,
    const jsg::TypeHandler<jsg::Promise<Result>>& resultPromiseHandler,
    Args... args) {
  auto rpcPromise =
      callR2RpcMethod(js, kj::mv(rpcProp), rpcPropHandler, fnHandler, kj::mv(args)...);
  return unwrapR2RpcPromise<Result>(js, kj::mv(rpcPromise), resultPromiseHandler);
}

template <typename Result, typename... Args>
jsg::Promise<Result> callR2RpcMethod(jsg::Lock& js,
    R2RpcClient& client,
    kj::StringPtr methodName,
    const jsg::TypeHandler<jsg::Ref<JsRpcProperty>>& rpcPropHandler,
    const jsg::TypeHandler<jsg::Function<jsg::Value(Args...)>>& fnHandler,
    const jsg::TypeHandler<jsg::Promise<Result>>& resultPromiseHandler,
    Args... args) {
  auto rpcPromise = client.call(js, methodName, rpcPropHandler, fnHandler, kj::mv(args)...);
  return unwrapR2RpcPromise<Result>(js, kj::mv(rpcPromise), resultPromiseHandler);
}

// NOTE: We don't currently actually use this as a structured object (hence the `kj::Own<R2Error>`
// that we see pop up).
// TODO(soon): Switch to structured objects and use jsg::Ref<R2Error> instead of kj::Own<R2Error>
//   to maintain ownership.
class R2Error: public jsg::Object {
 public:
  R2Error(uint v4Code, kj::String message): v4Code(v4Code), message(kj::mv(message)) {}

  constexpr kj::StringPtr getName() const {
    return "R2Error"_kj;
  }
  uint getV4Code() const {
    return v4Code;
  }
  kj::StringPtr getMessage() const {
    return message;
  }
  kj::StringPtr getAction() const {
    return KJ_ASSERT_NONNULL(action);
  }
  jsg::JsValue getStack(jsg::Lock& js);

  JSG_RESOURCE_TYPE(R2Error) {
    JSG_INHERIT_INTRINSIC(v8::kErrorPrototype);

    JSG_READONLY_INSTANCE_PROPERTY(name, getName);
    JSG_READONLY_INSTANCE_PROPERTY(code, getV4Code);
    JSG_READONLY_INSTANCE_PROPERTY(message, getMessage);
    JSG_READONLY_INSTANCE_PROPERTY(action, getAction);

    JSG_READONLY_INSTANCE_PROPERTY(stack, getStack);
    // See getStack in dom-exception.h

    JSG_TS_ROOT();
  }

 private:
  uint v4Code;
  kj::String message;
  kj::Maybe<kj::String> action;
  // Initialized when thrown.

  kj::Maybe<v8::Global<v8::Object>> errorForStack;
  // See dom-exception.h.

  friend struct R2Result;
};

using R2PutValue =
    kj::OneOf<JsReadableStream, kj::Array<kj::byte>, jsg::NonCoercible<kj::String>, jsg::Ref<Blob>>;
using R2PutValueRpc = kj::OneOf<JsReadableStream, kj::Array<kj::byte>, kj::String, jsg::Ref<Blob>>;

struct PreparedR2RpcBody {
  R2PutValueRpc value;
  double size;
};

// Prepares the transport value and the exact byte length passed alongside it. The length is an
// internal argument to the gateway's named RPC method, not part of the public R2 API.
PreparedR2RpcBody prepareR2RpcBody(jsg::Lock& js, R2PutValue value);

struct R2Result {
  uint httpStatus;

  // Non-null if httpStatus >= 400.
  kj::Maybe<kj::Own<R2Error>> toThrow;

  kj::Maybe<kj::Array<char>> metadataPayload;

  kj::Maybe<kj::Own<workerd::api::ReadableStreamSource>> stream;

  bool objectNotFound() {
    return httpStatus == 404 && v4ErrorCode() == 10007;
  }

  bool preconditionFailed() {
    return httpStatus == 412 && (v4ErrorCode() == 10031 || v4ErrorCode() == 10032);
  }

  bool success() {
    return httpStatus >= 200 && httpStatus < 400;
  }

  kj::Maybe<uint> v4ErrorCode();
  kj::Maybe<kj::String> getR2ErrorMessage();
  void throwIfError(kj::StringPtr action, const jsg::TypeHandler<jsg::Ref<R2Error>>& errorType);
};

kj::Promise<R2Result> doR2HTTPGetRequest(kj::Own<kj::HttpClient> client,
    kj::String metadataPayload,
    kj::ArrayPtr<kj::StringPtr> path,
    kj::Maybe<kj::StringPtr> jwt,
    CompatibilityFlags::Reader flags);

// Note: takes a jsg::Lock because computing the expected body size of a JsReadableStream value
// requires it. The lock is used synchronously (before any I/O) and is not retained.
kj::Promise<R2Result> doR2HTTPPutRequest(jsg::Lock& js,
    kj::Own<kj::HttpClient> client,
    kj::Maybe<R2PutValue> value,
    kj::Maybe<uint64_t> streamSize,
    // Deprecated. For internal beta API only.
    kj::String metadataPayload,
    kj::ArrayPtr<kj::StringPtr> path,
    kj::Maybe<kj::StringPtr> jwt);

}  // namespace workerd::api
