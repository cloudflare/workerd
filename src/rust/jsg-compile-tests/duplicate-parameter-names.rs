// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Example;

#[jsg_resource]
impl Example {
    #[jsg_method]
    fn method(&self, _a: String, a: String) {
        //~^ ERROR duplicate TypeScript parameter name `a`
    }
}
