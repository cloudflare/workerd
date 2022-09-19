// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "common.h"

namespace workerd::api {

WritableStreamController::PendingAbort::PendingAbort(
    jsg::Lock& js,
    jsg::PromiseResolverPair<void> prp,
    v8::Local<v8::Value> reason,
    bool reject)
    : resolver(kj::mv(prp.resolver)),
      promise(kj::mv(prp.promise)),
      reason(js.v8Ref(reason)),
      reject(reject) {}

WritableStreamController::PendingAbort::PendingAbort(
    jsg::Lock& js,
    v8::Local<v8::Value> reason, bool reject)
    : WritableStreamController::PendingAbort(
          js,
          js.newPromiseAndResolver<void>(),
          reason,
          reject) {}

void WritableStreamController::PendingAbort::complete(jsg::Lock& js) {
  if (reject) {
    fail(reason.getHandle(js));
  } else {
    maybeResolvePromise(resolver);
  }
}

void WritableStreamController::PendingAbort::fail(v8::Local<v8::Value> reason) {
  maybeRejectPromise<void>(resolver, reason);
}

kj::Maybe<WritableStreamController::PendingAbort>
WritableStreamController::PendingAbort::dequeue(
    kj::Maybe<WritableStreamController::PendingAbort>& maybePendingAbort) {
  return kj::mv(maybePendingAbort);
}

}  // namespace workerd::api
