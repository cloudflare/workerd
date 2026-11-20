// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `&LoopChecked` would let another thread use the value, so it is not `Send`.
fn require_send<T: Send>(_: T) {}

pub fn t(value: &kj_rs::LoopChecked<u32>) {
    require_send(value); //~ ERROR cannot be shared between threads safely
}
