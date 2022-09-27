// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/test.h>
#include <workerd/api/global-scope.h>
#include <workerd/jsg/rtti.h>

// Test building rtti for various APIs.

namespace workerd::api {
namespace {

KJ_TEST("WorkerGlobalScope") {
  CompatibilityFlags::Reader flags;
  jsg::rtti::Builder(flags).structure<WorkerGlobalScope>();
}

KJ_TEST("ServiceWorkerGlobalScope") {
  CompatibilityFlags::Reader flags;
  jsg::rtti::Builder(flags).structure<ServiceWorkerGlobalScope>();
}

} // namespace
} // namespace workerd::api
