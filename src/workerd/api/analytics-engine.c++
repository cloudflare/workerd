// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "analytics-engine.h"

#include <workerd/api/analytics-engine-impl.h>
#include <workerd/api/analytics-engine.capnp.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

namespace {

constexpr kj::StringPtr ERROR_PREFIX = "writeDataPoint(): "_kj;

// Throws what writeDataPointNoOutputLock() would for `event`, in the same order.
void validateDataPoint(const AnalyticsEngine::AnalyticsEngineEvent& event) {
  KJ_IF_SOME(indexes, event.indexes) {
    validateIndexes(indexes, ERROR_PREFIX);
  }
  KJ_IF_SOME(blobs, event.blobs) {
    validateBlobs(blobs, ERROR_PREFIX);
  }
  KJ_IF_SOME(doubles, event.doubles) {
    validateDoubles(doubles, ERROR_PREFIX);
  }
}

}  // namespace

void AnalyticsEngine::writeDataPoint(
    jsg::Lock& js, jsg::Optional<api::AnalyticsEngine::AnalyticsEngineEvent> event) {
  auto& context = IoContext::current();

  context.getLimitEnforcer().newAnalyticsEngineRequest();

  // A data point that cannot be written throws here, rather than where its message is built: in a
  // Durable Object that happens after the output locks, and an IoChannelFactory::writeLogfwdr()
  // may build it inside a promise, either of which would leave the error uncaught. The message,
  // and so its timestamp, is still built only when it is written.
  KJ_IF_SOME(ev, event) {
    validateDataPoint(ev);
  }

  // Optimization: For non-actors, which never have output locks, avoid the overhead of
  // awaitIo() and such by not going back to the event loop at all.
  KJ_IF_SOME(promise, context.waitForOutputLocksIfNecessary()) {
    context.awaitIo(
        js, kj::mv(promise), [self = JSG_THIS, event = kj::mv(event)](jsg::Lock& js) mutable {
      self->writeDataPointNoOutputLock(js, kj::mv(event));
    });
  } else {
    writeDataPointNoOutputLock(js, kj::mv(event));
  }
}

void AnalyticsEngine::writeDataPointNoOutputLock(
    jsg::Lock& js, jsg::Optional<api::AnalyticsEngine::AnalyticsEngineEvent>&& event) {
  auto& context = IoContext::current();

  context.writeLogfwdr(logfwdrChannel, [&](capnp::AnyPointer::Builder ptr) {
    api::AnalyticsEngineEvent::Builder aeEvent = ptr.initAs<api::AnalyticsEngineEvent>();

    aeEvent.setAccountId(static_cast<int64_t>(ownerId));
    aeEvent.setTimestamp(now());
    aeEvent.setDataset(dataset.asBytes());
    aeEvent.setSchemaVersion(version);
    // `index1` should default to the empty string (`""`).
    // The optional call to `setIndexes()`below assumes defaults, if any.
    aeEvent.setIndex1(""_kj.asBytes());

    KJ_IF_SOME(ev, event) {
      KJ_IF_SOME(indexes, ev.indexes) {
        setIndexes<api::AnalyticsEngineEvent::Builder>(aeEvent, indexes, ERROR_PREFIX);
      }
      KJ_IF_SOME(blobs, ev.blobs) {
        setBlobs<api::AnalyticsEngineEvent::Builder>(aeEvent, blobs, ERROR_PREFIX);
      }
      KJ_IF_SOME(doubles, ev.doubles) {
        setDoubles<api::AnalyticsEngineEvent::Builder>(aeEvent, doubles, ERROR_PREFIX);
      }
    }
  });
}
kj::Maybe<kj::Own<jsg::Wrappable>> AnalyticsEngine::snapshotClone() const {
  return ownAsWrappable(
      kj::refcounted<AnalyticsEngine>(logfwdrChannel, kj::str(dataset), version, ownerId));
}

}  // namespace workerd::api
