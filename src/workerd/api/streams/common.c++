// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "common.h"

#include <workerd/io/features.h>

namespace workerd::api {

WritableStreamController::PendingAbort::PendingAbort(
    jsg::Lock& js, jsg::PromiseResolverPair<void> prp, jsg::JsValue reason, bool reject)
    : resolver(kj::mv(prp.resolver)),
      promise(kj::mv(prp.promise)),
      reason(reason.addRef(js)),
      reject(reject) {}

WritableStreamController::PendingAbort::PendingAbort(
    jsg::Lock& js, jsg::JsValue reason, bool reject)
    : WritableStreamController::PendingAbort(js, js.newPromiseAndResolver<void>(), reason, reject) {
}

void WritableStreamController::PendingAbort::complete(jsg::Lock& js) {
  if (reject) {
    fail(js, reason.getHandle(js));
  } else {
    maybeResolvePromise(js, resolver);
  }
}

void WritableStreamController::PendingAbort::fail(jsg::Lock& js, jsg::JsValue reason) {
  maybeRejectPromise<void>(js, resolver, reason);
}

kj::Maybe<kj::Own<WritableStreamController::PendingAbort>> WritableStreamController::PendingAbort::
    dequeue(kj::Maybe<kj::Own<WritableStreamController::PendingAbort>>& maybePendingAbort) {
  return kj::mv(maybePendingAbort);
}

// ====================================================================================

UnderlyingSinkImpl::UnderlyingSinkImpl(
    jsg::Lock& js, UnderlyingSink sink, StreamQueuingStrategy strategy)
    : start_(kj::mv(sink.start)),
      write_(kj::mv(sink.write)),
      abort_(kj::mv(sink.abort)),
      close_(kj::mv(sink.close)),
      size_(kj::mv(strategy.size)),
      highWaterMark_(strategy.highWaterMark.orDefault(DEFAULT_HIGH_WATER_MARK)) {
  // Per the streams spec, the size function should be called with `undefined` as `this`,
  // not as a method on the strategy object.
  KJ_IF_SOME(size, size_) {
    size.setReceiver(js.v8Ref(js.v8Undefined()));
  }
  if (FeatureFlags::get(js).getPedanticWpt()) {
    // Per the spec, the type property for WritableStream's underlying sink must be undefined.
    // If it's anything else, throw a RangeError.
    JSG_REQUIRE(sink.type == kj::none, RangeError,
        "Invalid underlying sink type. Only undefined is valid.");
  }
}

}  // namespace workerd::api
