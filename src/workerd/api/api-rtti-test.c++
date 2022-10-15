// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/test.h>
#include <workerd/api/actor.h>
#include <workerd/api/global-scope.h>
#include <workerd/jsg/rtti.h>

// Test building rtti for various APIs.

namespace workerd::api {
namespace {

KJ_TEST("WorkerGlobalScope") {
  jsg::rtti::Builder builder((CompatibilityFlags::Reader()));
  builder.structure<WorkerGlobalScope>();
  KJ_EXPECT(builder.structure("workerd::api::Event"_kj) != nullptr);
  KJ_EXPECT(builder.structure("workerd::api::ObviouslyWrongName"_kj) == nullptr);
}

KJ_TEST("ServiceWorkerGlobalScope") {
  jsg::rtti::Builder builder((CompatibilityFlags::Reader()));
  builder.structure<ServiceWorkerGlobalScope>();
  KJ_EXPECT(builder.structure("workerd::api::DurableObjectId"_kj) != nullptr);
}

} // namespace
} // namespace workerd::api
