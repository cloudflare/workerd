// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/global-scope.h>

namespace workerd::api {

// Whether Worker::Api::newExecutionContext() creates Rust ExecutionContexts (lib.rs) rather than
// C++ ones: the `rust-execution-context` autogate. An implementation of Worker::Api that can
// create them calls newRustExecutionContext() when this is true, and overrides
// Worker::Api::isRustExecutionContext() with isRustExecutionContext().
bool rustExecutionContextEnabled();

// Creates a Rust ExecutionContext, the `ctx` of a stateless entrypoint.
exportedhandler::ExecutionContext newRustExecutionContext(jsg::Lock& js,
    jsg::JsValue exports,
    jsg::JsValue props,
    kj::Maybe<Worker::VersionInfo> versionInfo);

// Whether `value` is a Rust ExecutionContext.
bool isRustExecutionContext(jsg::Lock& js, v8::Local<v8::Value> value);

}  // namespace workerd::api
