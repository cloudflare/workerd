// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "request-io.h"

#include "basics.h"
#include "http.h"

namespace workerd::api {

jsg::JsValue RequestIo::createForBootstrap(jsg::Lock& js) {
  if (!IoContext::hasCurrent()) return js.undefined();
  // RequestIo is registered in EW_HTTP_ISOLATE_TYPES.
  auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<RequestIo>>(),
      "RequestIo is missing from the isolate type list");
  return jsg::JsValue(handler.wrap(js, js.alloc<RequestIo>(IoContext::current().getId())));
}

bool RequestIo::isCurrent() {
  return contextId.isCurrent();
}

void RequestIo::requireCurrent() {
  IoContext::requireCurrentOrThrowJs(contextId);
}

jsg::Promise<jsg::Ref<Response>> RequestIo::fetch(jsg::Lock& js,
    kj::Maybe<jsg::Ref<Fetcher>> fetcher,
    kj::String url,
    jsg::Optional<RequestInitializerDict> init) {
  requireCurrent();
  jsg::Optional<Request::Initializer> requestInit;
  KJ_IF_SOME(dict, init) {
    requestInit = Request::Initializer(kj::mv(dict));
  }
  return fetchImpl(js, kj::mv(fetcher), kj::mv(url), kj::mv(requestInit));
}

jsg::Promise<void> RequestIo::wait(
    jsg::Lock& js, uint32_t milliseconds, jsg::Ref<AbortSignal> signal) {
  requireCurrent();
  auto& context = IoContext::current();
  return context.awaitIo(
      js, signal->wrap(js, context.afterLimitTimeout(milliseconds * kj::MILLISECONDS)));
}

void RequestIo::logUncaughtException(jsg::Lock& js, jsg::JsValue error) {
  requireCurrent();
  IoContext::current().logUncaughtException(UncaughtExceptionSource::ASYNC_TASK, error);
}

}  // namespace workerd::api
