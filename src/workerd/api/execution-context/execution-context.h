// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/exported-handler-ctx.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

// Creates an ExecutionContext (lib.rs), the `ctx` of a stateless entrypoint, for an
// implementation of Worker::Api::newExecutionContext().
exportedhandler::ExecutionContext newExecutionContext(jsg::Lock& js,
    jsg::JsValue exports,
    jsg::JsValue props,
    kj::Maybe<Worker::VersionInfo> versionInfo);

// Whether `value` is an ExecutionContext, for an implementation of
// Worker::Api::isExecutionContext().
bool isExecutionContext(jsg::Lock& js, v8::Local<v8::Value> value);

}  // namespace workerd::api
