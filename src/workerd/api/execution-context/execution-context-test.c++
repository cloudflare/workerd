// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "execution-context.h"

#include <workerd/api/global-scope.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

// Stores the `ctx` its class entrypoint is constructed with, so that the test can check it.
constexpr kj::StringPtr MAIN_MODULE = R"SCRIPT(
  import { WorkerEntrypoint } from 'cloudflare:workers';
  export default {
    fetch(request) { return new Response("OK"); },
  };
  export class Stateless extends WorkerEntrypoint {
    constructor(ctx, env) {
      super(ctx, env);
      globalThis.statelessCtx = ctx;
    }
  }
)SCRIPT"_kj;

KJ_TEST("WorkerdApi creates the ctx of handlers and class entrypoints as ExecutionContexts") {
  TestFixture fixture({.mainModuleSource = MAIN_MODULE});
  // A request has one entrypoint handler, so each entrypoint gets a request of its own.
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto handler = KJ_ASSERT_NONNULL(env.lock.getExportedHandler(kj::none, kj::none, {}, kj::none));
    auto ctx = KJ_ASSERT_NONNULL(handler->getCtx(js)).getHandle(js);
    KJ_EXPECT(env.lock.getWorker().getIsolate().getApi().isExecutionContext(js, ctx));
  });
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto& api = env.lock.getWorker().getIsolate().getApi();
    KJ_ASSERT_NONNULL(env.lock.getExportedHandler("Stateless"_kj, kj::none, {}, kj::none));
    KJ_EXPECT(api.isExecutionContext(js, js.global().get(js, "statelessCtx"_kj)));
    KJ_EXPECT(!api.isExecutionContext(js, js.obj()));
  });
}

}  // namespace
}  // namespace workerd::api
