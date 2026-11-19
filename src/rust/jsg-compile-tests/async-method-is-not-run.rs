// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An async method's future converts to JavaScript like any other return value, and a future is
// not `ToJS`. Were it taken for a unit return, the callback would drop it without running it.

use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Example;

#[jsg_resource]
impl Example {
    #[jsg_method]
    async fn close(&self) {}
    //~? ERROR E0277
}
