// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "analytics-engine.h"

#include <workerd/api/analytics-engine-impl.h>
#include <workerd/api/analytics-engine.capnp.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

void AnalyticsEngine::writeDataPoint(
    jsg::Lock& js, jsg::Optional<api::AnalyticsEngine::AnalyticsEngineEvent> event) {
  auto& context = IoContext::current();

  context.getLimitEnforcer().newAnalyticsEngineRequest();

  // Optimization: For non-actors, which never have output locks, avoid the overhead of
  // awaitIo() and such by not going back to the event loop at all.
  KJ_IF_SOME(promise, context.waitForOutputLocksIfNecessary()) {
    context.awaitIo(js, kj::mv(promise), [this, event = kj::mv(event)](jsg::Lock& js) mutable {
      writeDataPointNoOutputLock(js, kj::mv(event));
    });
  } else {
    writeDataPointNoOutputLock(js, kj::mv(event));
  }
}

void AnalyticsEngine::writeDataPointNoOutputLock(
    jsg::Lock& js, jsg::Optional<api::AnalyticsEngine::AnalyticsEngineEvent>&& event) {
  auto& context = IoContext::current();
  auto userSpan = context.makeUserTraceSpan("ae_writeDataPoint"_kjc);

  context.writeLogfwdr(logfwdrChannel, [&](capnp::AnyPointer::Builder ptr) {
    api::AnalyticsEngineEvent::Builder aeEvent = ptr.initAs<api::AnalyticsEngineEvent>();

    userSpan.setTag("db.namespace"_kjc, kj::str(dataset));

    aeEvent.setAccountId(static_cast<int64_t>(ownerId));
    aeEvent.setTimestamp(now());
    aeEvent.setDataset(dataset.asBytes());
    aeEvent.setSchemaVersion(version);
    // `index1` should default to the empty string (`""`).
    // The optional call to `setIndexes()`below assumes defaults, if any.
    aeEvent.setIndex1(""_kj.asBytes());

    kj::StringPtr errorPrefix = "writeDataPoint(): "_kj;
    KJ_IF_SOME(ev, event) {
      KJ_IF_SOME(indexes, ev.indexes) {
        KJ_IF_SOME(index, indexes[0]) {
          userSpan.setTag("cloudflare.wae.query.index"_kjc, kj::str(index));
        }
        setIndexes<api::AnalyticsEngineEvent::Builder>(aeEvent, indexes, errorPrefix);
      }
      KJ_IF_SOME(blobs, ev.blobs) {
        //cast to int64_t
        userSpan.setTag("cloudflare.wae.query.blobs"_kjc, static_cast<int64_t>(blobs.size()));
        setBlobs<api::AnalyticsEngineEvent::Builder>(aeEvent, blobs, errorPrefix);
      }
      KJ_IF_SOME(doubles, ev.doubles) {
        userSpan.setTag("cloudflare.wae.query.doubles"_kjc, static_cast<int64_t>(doubles.size()));
        setDoubles<api::AnalyticsEngineEvent::Builder>(aeEvent, doubles, errorPrefix);
      }
    }
  });
}
}  // namespace workerd::api
