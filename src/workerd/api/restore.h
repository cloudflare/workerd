// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Code related to `ctx.restore()` and the `[restore]()` method.

#include <workerd/jsg/jsg.h>

namespace workerd::api {

class Fetcher;
class JsRpcStub;

// Implementation of ctx.restore(). Invokes `[restore](params)` on the current entrypoint,
// constructs the appropriate channel token for the result, and returns a Fetcher or JsRpcStub
// imbued with that token.
jsg::Promise<jsg::Value> restoreCurrentEntrypoint(jsg::Lock& js,
    jsg::JsObject params,
    const jsg::TypeHandler<jsg::Ref<Fetcher>>& fetcherHandler,
    const jsg::TypeHandler<jsg::Ref<JsRpcStub>>& rpcStubHandler);

};  // namespace workerd::api
