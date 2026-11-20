// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "execution-context.h"

#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

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

struct CtxKinds {
  // The `ctx` of the default (object) handler.
  bool handlerCtxIsRust;
  // The `ctx` passed to the constructor of the class entrypoint.
  bool classCtxIsRust;
  // A plain object, which is neither kind.
  bool plainObjectIsRust;
};

// Which kind of ExecutionContext WorkerdApi creates, with the rust-execution-context autogate
// enabled or not, whatever WORKERD_ALL_AUTOGATES says.
CtxKinds createdCtxKinds(bool rustExecutionContext) {
  TestFixture fixture({
    .autogateKeys = rustExecutionContext ? kj::arr(util::AutogateKey::RUST_EXECUTION_CONTEXT)
                                         : kj::Array<util::AutogateKey>(),
    .ignoreAllAutogatesEnv = true,
    .mainModuleSource = MAIN_MODULE,
  });
  KJ_ASSERT(rustExecutionContextEnabled() == rustExecutionContext);
  // A request has one entrypoint handler, so each entrypoint gets a request of its own.
  bool handlerCtxIsRust = fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto handler = KJ_ASSERT_NONNULL(env.lock.getExportedHandler(kj::none, kj::none, {}, kj::none));
    auto ctx = KJ_ASSERT_NONNULL(handler->getCtx(js)).getHandle(js);
    return env.lock.getWorker().getIsolate().getApi().isRustExecutionContext(js, ctx);
  });
  return fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto& api = env.lock.getWorker().getIsolate().getApi();
    KJ_ASSERT_NONNULL(env.lock.getExportedHandler("Stateless"_kj, kj::none, {}, kj::none));
    auto classCtx = js.global().get(js, "statelessCtx"_kj);
    return CtxKinds{
      .handlerCtxIsRust = handlerCtxIsRust,
      .classCtxIsRust = api.isRustExecutionContext(js, classCtx),
      .plainObjectIsRust = api.isRustExecutionContext(js, js.obj()),
    };
  });
}

KJ_TEST("the rust-execution-context autogate makes WorkerdApi create Rust ExecutionContexts") {
  auto kinds = createdCtxKinds(true);
  KJ_EXPECT(kinds.handlerCtxIsRust);
  KJ_EXPECT(kinds.classCtxIsRust);
  KJ_EXPECT(!kinds.plainObjectIsRust);
}

KJ_TEST("without the rust-execution-context autogate, WorkerdApi creates C++ ExecutionContexts") {
  auto kinds = createdCtxKinds(false);
  KJ_EXPECT(!kinds.handlerCtxIsRust);
  KJ_EXPECT(!kinds.classCtxIsRust);
  KJ_EXPECT(!kinds.plainObjectIsRust);
}

}  // namespace
}  // namespace workerd::api
