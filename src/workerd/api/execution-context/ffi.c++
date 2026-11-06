// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include "execution-context.h"

#include <workerd/api/execution-context/ffi.rs.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/http.h>
#include <workerd/api/restore.h>
#include <workerd/api/tracing.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/io/access-info.h>
#include <workerd/io/io-context.h>
#include <workerd/util/autogate.h>

namespace workerd::api {

bool rustExecutionContextEnabled() {
  return util::Autogate::isEnabled(util::AutogateKey::RUST_EXECUTION_CONTEXT);
}

exportedhandler::ExecutionContext newRustExecutionContext(jsg::Lock& js,
    jsg::JsValue exports,
    jsg::JsValue props,
    kj::Maybe<Worker::VersionInfo> versionInfo) {
  using ::workerd::rust::jsg::maybe_local_from_ffi;
  using ::workerd::rust::jsg::maybe_local_to_ffi;
  using ::workerd::rust::jsg::to_ffi;
  v8::MaybeLocal<v8::Value> version;
  KJ_IF_SOME(v, ExecutionContext::versionToJs(js, kj::mv(versionInfo))) {
    version = v8::Local<v8::Value>(v);
  }
  auto created =
      execution_context::execution_context_new(js.v8Isolate, to_ffi(v8::Local<v8::Value>(exports)),
          to_ffi(v8::Local<v8::Value>(props)), maybe_local_to_ffi(version));
  v8::Local<v8::Value> wrapper;
  if (!maybe_local_from_ffi<v8::Value>(kj::mv(created)).ToLocal(&wrapper)) {
    throw jsg::JsExceptionThrown();
  }
  return exportedhandler::ExecutionContext::fromRust(js, jsg::JsObject(wrapper.As<v8::Object>()));
}

bool isRustExecutionContext(jsg::Lock& js, v8::Local<v8::Value> value) {
  KJ_IF_SOME(is,
      execution_context::execution_context_is(
          js.v8Isolate, ::workerd::rust::jsg::to_ffi(v8::Local<v8::Value>(value)))) {
    return is;
  }
  // The check failed with an exception pending.
  throw jsg::JsExceptionThrown();
}

}  // namespace workerd::api

namespace workerd::api::execution_context {

kj::Maybe<kj::Own<jsg_cpp::CppRef>> fetcher_try_unwrap(v8::Isolate* isolate, jsg_cpp::Local value) {
  return jsg_cpp::cppRefTryUnwrap<Fetcher>(isolate, kj::mv(value));
}

jsg_cpp::MaybeLocal fetcher_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref) {
  return jsg_cpp::cppRefWrap<Fetcher>(isolate, ref);
}

kj::Maybe<kj::Own<jsg_cpp::CppRef>> tracing_try_unwrap(v8::Isolate* isolate, jsg_cpp::Local value) {
  return jsg_cpp::cppRefTryUnwrap<Tracing>(isolate, kj::mv(value));
}

jsg_cpp::MaybeLocal tracing_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref) {
  return jsg_cpp::cppRefWrap<Tracing>(isolate, ref);
}

kj::Maybe<kj::Own<jsg_cpp::CppRef>> cache_context_try_unwrap(
    v8::Isolate* isolate, jsg_cpp::Local value) {
  return jsg_cpp::cppRefTryUnwrap<CacheContext>(isolate, kj::mv(value));
}

jsg_cpp::MaybeLocal cache_context_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref) {
  return jsg_cpp::cppRefWrap<CacheContext>(isolate, ref);
}

kj::Maybe<kj::Own<jsg_cpp::CppRef>> access_context_try_unwrap(
    v8::Isolate* isolate, jsg_cpp::Local value) {
  return jsg_cpp::cppRefTryUnwrap<AccessContext>(isolate, kj::mv(value));
}

jsg_cpp::MaybeLocal access_context_wrap(v8::Isolate* isolate, const jsg_cpp::CppRef& ref) {
  return jsg_cpp::cppRefWrap<AccessContext>(isolate, ref);
}

// Rust's `&T` of an opaque type is interior-mutable, so the shims may call non-const methods.
bool fetcher_map_virtual_host(
    v8::Isolate* isolate, const Fetcher& fetcher, uint16_t port, ::rust::String& result) {
  return jsg_cpp::callCpp(isolate, [&](jsg::Lock& js) {
    auto host = const_cast<Fetcher&>(fetcher).mapVirtualHost(js, port);
    result = ::rust::String(host.cStr(), host.size());
  });
}

bool new_tracing(v8::Isolate* isolate, kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result) {
  // Tracing is stateless: the per-request state lives in the IoContext, which its methods
  // consult when called.
  return jsg_cpp::callCppForRef<Tracing>(
      isolate, result, [](jsg::Lock& js) { return js.alloc<Tracing>(); });
}

bool current_cache_context(v8::Isolate* isolate, kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result) {
  return jsg_cpp::callCppForRef<CacheContext>(
      isolate, result, [](jsg::Lock& js) -> jsg::Optional<jsg::Ref<CacheContext>> {
    if (!IoContext::hasCurrent()) return kj::none;
    return Worker::Isolate::from(js).getApi().getCtxCacheProperty(js);
  });
}

bool current_access_context(v8::Isolate* isolate, kj::Maybe<kj::Own<jsg_cpp::CppRef>>& result) {
  return jsg_cpp::callCppForRef<AccessContext>(
      isolate, result, [](jsg::Lock& js) -> jsg::Optional<jsg::Ref<AccessContext>> {
    if (!IoContext::hasCurrent()) return kj::none;
    auto& ioctx = IoContext::current();
    KJ_IF_SOME(info, ioctx.getAccessInfo()) {
      return js.alloc<AccessContext>(ioctx.addObject(kj::addRef(info)));
    }
    return kj::none;
  });
}

namespace {
// The reaction that C++ JSG adds to a returned jsg::Promise<jsg::Value> to convert its value to
// JavaScript, which for a jsg::Value passes it on as it is.
void passValueOn(const v8::FunctionCallbackInfo<v8::Value>& args) {
  args.GetReturnValue().Set(args[0]);
}
}  // namespace

jsg_cpp::MaybeLocal restore_current_entrypoint(v8::Isolate* isolate, jsg_cpp::Local params) {
  v8::MaybeLocal<v8::Value> result;
  bool succeeded = jsg_cpp::callCpp(isolate, [&](jsg::Lock& js) {
    auto& fetcherHandler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<Fetcher>>());
    auto& rpcStubHandler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<JsRpcStub>>());
    auto promise = restoreCurrentEntrypoint(js,
        jsg::JsObject(::workerd::rust::jsg::local_from_ffi<v8::Object>(kj::mv(params))),
        fetcherHandler, rpcStubHandler);
    // Returns the promise through a reaction, as C++ JSG returns a jsg::Promise<jsg::Value>, so
    // that it settles in the same microtask as with the C++ ExecutionContext.
    auto context = js.v8Context();
    auto then = jsg::check(
        v8::Function::New(context, &passValueOn, {}, 1, v8::ConstructorBehavior::kThrow));
    result = jsg::check(promise.consumeHandle(js)->Then(context, then));
  });
  if (!succeeded) {
    // An exception is pending.
    return ::workerd::rust::jsg::maybe_local_to_ffi(v8::MaybeLocal<v8::Value>());
  }
  return ::workerd::rust::jsg::maybe_local_to_ffi(result);
}

}  // namespace workerd::api::execution_context
