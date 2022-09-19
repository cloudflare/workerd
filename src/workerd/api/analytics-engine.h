// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/util.h>
#include <workerd/api/analytics-engine-impl.h>
#include <workerd/jsg/jsg.h>
#include <workerd/api/analytics-engine.capnp.h>
#include <kj/async.h>

namespace workerd::api {

using kj::byte;

class AnalyticsEngine: public jsg::Object {
  // Analytics Engine is a tool for customers to get telemetry about anything
  // using Workers. The data points gathered from the edge are stored into
  // ClickHouse and can be queried through the Analytics Engine's SQL API.
  //
  // The generated data points are encoded through the
  // analytics_engine_event.capnp format and sent to logfwdr for them to enter
  // the Data Pipeline. Each data point consists of an array of index values, 20
  // numeric fields (doubles) and 20 text fields (blobs), alongside some
  // metadata. Aside from ordinality and maximum length, the semantics of the
  // `blobs` and `doubles` fields are left up to applications submitting
  // messages.
  //
  // This project's desing closely resembles that of its dual internal tool,
  // Ready Analytics. See this Ready Analytics FAQ for more information:
  // https://wiki.cfops.it/display/DATA/Ready+Analytics+-+FAQ
  // Also, see this doc on how the two differ:
  // https://wiki.cfops.it/display/~jpl/Understanding+Ready+Analytics+and+Workers+Analytics+Engine

public:
  explicit AnalyticsEngine(uint logfwdrChannel, kj::String dataset,
                           int64_t version, uint32_t ownerId)
      : logfwdrChannel(logfwdrChannel), dataset(kj::mv(dataset)),
        version(version), ownerId(ownerId) {}
  struct AnalyticsEngineEvent {
    jsg::Optional<kj::Array<kj::Maybe<kj::OneOf<kj::Array<byte>, kj::String>>>> indexes;
    // An array of values for the user-defined indexes, that provide a way for
    // users to improve the efficiency of common queries. In addition, by
    // default, the sampling key includes all the indexes in the list. This
    // gives users some control over the way data is sampled.
    jsg::Optional<kj::Array<double>> doubles;
    jsg::Optional<kj::Array<kj::Maybe<kj::OneOf<kj::Array<byte>, kj::String>>>> blobs;
    // The ordering of the elements within the `doubles` and `blobs` fields matters, insofar as the
    // elements within each array will be unrolled, and based on the element's ordinality, the
    // `.set{Blob,Data}{1..20}(element)` method is invoked. Because of this, these arrays can each
    // have a maximum of 20 elements.

    JSG_STRUCT(indexes, doubles, blobs);
  };

  void writeDataPoint(jsg::Lock& js,
             jsg::Optional<api::AnalyticsEngine::AnalyticsEngineEvent> event);
  // Send an Analytics Engine-compatible event to the configured logfwdr socket.
  // Like logfwdr itself, `writeDataPoint` makes no delivery guarantees.

  JSG_RESOURCE_TYPE(AnalyticsEngine) {
    JSG_METHOD(writeDataPoint);
  }

private:
  double millisToNanos(double m) {
    return m * 1000000;
  }

  uint logfwdrChannel;
  kj::String dataset;
  int64_t version;
  uint32_t ownerId;

  uint64_t now() { return millisToNanos(dateNow()); }
};
#define EW_ANALYTICS_ENGINE_ISOLATE_TYPES                                                          \
  ::workerd::api::AnalyticsEngine, ::workerd::api::AnalyticsEngine::AnalyticsEngineEvent

}  // namespace workerd::api
