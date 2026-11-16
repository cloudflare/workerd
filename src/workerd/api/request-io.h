// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

class AbortSignal;
class Fetcher;
class Response;
struct RequestInitializerDict;

// The I/O of an API implemented in the per-isolate bootstrap (src/per_isolate/), bound to the
// request that created it, as a C++ API's I/O is bound to its IoContext. Obtained only from the
// bootstrap's utils.createRequestIo(); the bootstrap never lets one reach user code. Every method
// but isCurrent() throws, as IoContext::requireCurrentOrThrowJs() does, unless called within the
// request it is bound to.
class RequestIo final: public jsg::Object {
 public:
  explicit RequestIo(IoContext::Id contextId): contextId(contextId) {}

  // A RequestIo bound to the current request, or undefined outside of one.
  static jsg::JsValue createForBootstrap(jsg::Lock& js);

  // Whether this is running within the request this is bound to.
  bool isCurrent();

  // Throws unless running within the request this is bound to.
  void requireCurrent();

  // Fetches `url` with `init` through `fetcher`, or as the global fetch() does when there is none.
  jsg::Promise<jsg::Ref<Response>> fetch(jsg::Lock& js,
      kj::Maybe<jsg::Ref<Fetcher>> fetcher,
      kj::String url,
      jsg::Optional<RequestInitializerDict> init);

  // Resolves after `milliseconds` on the request's after-limit timer, or rejects with `signal`'s
  // reason when it aborts first.
  jsg::Promise<void> wait(jsg::Lock& js, uint32_t milliseconds, jsg::Ref<AbortSignal> signal);

  // Logs `error` as an exception uncaught by an asynchronous task of the request.
  void logUncaughtException(jsg::Lock& js, jsg::JsValue error);

  JSG_RESOURCE_TYPE(RequestIo) {
    JSG_METHOD(isCurrent);
    JSG_METHOD(requireCurrent);
    JSG_METHOD(fetch);
    JSG_METHOD(wait);
    JSG_METHOD(logUncaughtException);
    JSG_TS_OVERRIDE(type RequestIo = never);
  }

 private:
  IoContext::Id contextId;
};

}  // namespace workerd::api
