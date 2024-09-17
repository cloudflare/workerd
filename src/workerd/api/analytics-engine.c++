// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "analytics-engine.h"

#include <workerd/io/io-context.h>

namespace workerd::api {

void AnalyticsEngine::writeDataPoint(
    jsg::Lock& js, jsg::Optional<api::AnalyticsEngine::AnalyticsEngineEvent> event) {
  auto& context = IoContext::current();

  context.getLimitEnforcer().newAnalyticsEngineRequest();

  context.writeLogfwdr(logfwdrChannel, [&](capnp::AnyPointer::Builder ptr) {
    api::AnalyticsEngineEvent::Builder aeEvent = ptr.initAs<api::AnalyticsEngineEvent>();

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
        setIndexes<api::AnalyticsEngineEvent::Builder>(aeEvent, indexes, errorPrefix);
      }
      KJ_IF_SOME(blobs, ev.blobs) {
        setBlobs<api::AnalyticsEngineEvent::Builder>(aeEvent, blobs, errorPrefix);
      }
      KJ_IF_SOME(doubles, ev.doubles) {
        setDoubles<api::AnalyticsEngineEvent::Builder>(aeEvent, doubles, errorPrefix);
      }
    }
  });
}
}  // namespace workerd::api
