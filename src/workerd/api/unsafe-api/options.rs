// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The options of `UnsafeModule.evict()` and `evictAllDurableObjects()`.

// `#[jsg_struct]` binds each field to a local of the same name in the code it generates.
#![allow(
    non_snake_case,
    reason = "a `#[jsg_struct]` field is named as its JavaScript property is"
)]

use io_context::EvictWebSocketMode;
use jsg_macros::jsg_struct;

/// The options of an eviction, as C++ `UnsafeModule::EvictOptions`.
#[jsg_struct(ts_override = r#"{ webSockets?: "hibernate" | "close"; }"#)]
pub struct EvictOptions {
    /// What happens to the object's WebSockets: `"hibernate"`, the default, or `"close"`.
    pub webSockets: Option<String>,
}

/// The WebSocket mode that `options` ask for.
pub fn web_socket_mode(options: Option<EvictOptions>) -> jsg::Result<EvictWebSocketMode> {
    match options.and_then(|options| options.webSockets).as_deref() {
        None | Some("hibernate") => Ok(EvictWebSocketMode::HIBERNATE),
        Some("close") => Ok(EvictWebSocketMode::CLOSE),
        Some(_) => Err(jsg::Error::new_type_error(
            "options.webSockets must be \"hibernate\" or \"close\".",
        )),
    }
}
