// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for Worker::Lock::getExportedHandler() error behaviour, specifically
// the isDynamicDispatch path which surfaces user-configuration mistakes as JSG
// TypeErrors rather than internal log-only errors.

#include <workerd/api/global-scope.h>
#include <workerd/io/frankenvalue.h>
#include <workerd/io/worker.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd {
namespace {

// ---------------------------------------------------------------------------
// isDynamicDispatch = true: both error cases must throw a JSG TypeError.
// ---------------------------------------------------------------------------

KJ_TEST("getExportedHandler: DO class via dynamic dispatch throws JSG TypeError") {
  TestFixture fixture({
    .mainModuleSource = R"(
      import { DurableObject } from "cloudflare:workers";
      export class SomeActor extends DurableObject {}
      export default { async fetch(req) { return new Response("ok"); } }
    )"_kj,
  });

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT_THROW_MESSAGE(
        "jsg.TypeError: The entrypoint name SomeActor refers to a Durable Object class, but the "
        "incoming request is trying to invoke it as a stateless worker.",
        env.lock.getExportedHandler("SomeActor"_kj, kj::none, Frankenvalue{}, kj::none, true));
  });
}

KJ_TEST("getExportedHandler: missing entrypoint via dynamic dispatch throws JSG TypeError") {
  TestFixture fixture({
    .mainModuleSource = R"(
      export default { async fetch(req) { return new Response("ok"); } }
    )"_kj,
  });

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT_THROW_MESSAGE(
        "jsg.TypeError: The entrypoint name nonExistent was not found in this worker. Ensure the "
        "worker exports an entrypoint with that name.",
        env.lock.getExportedHandler("nonExistent"_kj, kj::none, Frankenvalue{}, kj::none, true));
  });
}

// ---------------------------------------------------------------------------
// isDynamicDispatch = false: both error cases must NOT throw a JSG TypeError
// (they log and then throw a non-JSG internal error via KJ_FAIL_ASSERT).
// ---------------------------------------------------------------------------

KJ_TEST("getExportedHandler: DO class via static dispatch throws internal error") {
  TestFixture fixture({
    .mainModuleSource = R"(
      import { DurableObject } from "cloudflare:workers";
      export class SomeActor extends DurableObject {}
      export default { async fetch(req) { return new Response("ok"); } }
    )"_kj,
  });

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    // LOG_ERROR_PERIODICALLY fires, then KJ_FAIL_ASSERT throws.
    // No JSG TypeError — the error is treated as internal.
    KJ_EXPECT_LOG(ERROR, "worker is not an actor but class name was requested; n = SomeActor");
    KJ_EXPECT_THROW_MESSAGE("worker_do_not_log; Unable to get exported handler",
        env.lock.getExportedHandler("SomeActor"_kj, kj::none, Frankenvalue{}, kj::none, false));
  });
}

KJ_TEST("getExportedHandler: missing entrypoint via static dispatch throws internal error") {
  TestFixture fixture({
    .mainModuleSource = R"(
      export default { async fetch(req) { return new Response("ok"); } }
    )"_kj,
  });

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    // LOG_ERROR_PERIODICALLY fires, then KJ_FAIL_ASSERT throws.
    // No JSG TypeError — the error is treated as internal.
    KJ_EXPECT_LOG(ERROR, "worker has no such named entrypoint; n = nonExistent");
    KJ_EXPECT_THROW_MESSAGE("worker_do_not_log; Unable to get exported handler",
        env.lock.getExportedHandler("nonExistent"_kj, kj::none, Frankenvalue{}, kj::none, false));
  });
}

}  // namespace
}  // namespace workerd
