// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A builtin module's callback returns a handle of the scope it is given.
use jsg::ToJS;
use jsg::v8;

pub fn register(registry: std::pin::Pin<&mut v8::ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry,
        "node-internal:example",
        |lock| vec![jsg::Number::new(1.0)].to_js(lock),
        jsg::modules::ModuleType::Internal,
    );
}
