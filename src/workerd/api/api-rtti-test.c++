// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/actor-state.h>
#include <workerd/api/actor.h>
#include <workerd/api/cache.h>
#include <workerd/api/crypto/crypto.h>
#include <workerd/api/encoding.h>
#include <workerd/api/events.h>
#include <workerd/api/eventsource.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/html-rewriter.h>
#include <workerd/api/queue.h>
#include <workerd/api/scheduled.h>
#include <workerd/api/sockets.h>
#include <workerd/api/sql.h>
#include <workerd/api/streams.h>
#include <workerd/api/streams/standard.h>
#include <workerd/api/trace.h>
#include <workerd/api/url-standard.h>
#include <workerd/api/urlpattern.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/rtti.h>

#include <kj/test.h>

// Test building rtti for various APIs.

namespace workerd::api {
namespace {

KJ_TEST("WorkerGlobalScope") {
  jsg::rtti::Builder builder((CompatibilityFlags::Reader()));
  builder.structure<WorkerGlobalScope>();
  KJ_EXPECT(builder.structure("workerd::api::Event"_kj) != kj::none);
  KJ_EXPECT(builder.structure("workerd::api::ObviouslyWrongName"_kj) == kj::none);
}

KJ_TEST("ServiceWorkerGlobalScope") {
  jsg::rtti::Builder builder((CompatibilityFlags::Reader()));
  builder.structure<ServiceWorkerGlobalScope>();
  KJ_EXPECT(builder.structure("workerd::api::DurableObjectId"_kj) != kj::none);
}

}  // namespace
}  // namespace workerd::api
