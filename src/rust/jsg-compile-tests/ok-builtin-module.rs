// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A builtin module's callback returns its exports resource, which registration wraps in the
// registry's scope.
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Exports;

#[jsg_resource]
impl Exports {
    #[jsg_method]
    pub fn answer(&self) -> u32 {
        42
    }
}

pub fn register(registry: std::pin::Pin<&mut v8::ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry,
        "node-internal:example",
        |_lock| Ok(jsg::Rc::new(Exports)),
        jsg::modules::ModuleType::Internal,
    );
}
