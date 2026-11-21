// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The output of an `await_js_local` conversion belongs to the IoContext's thread, so the
// future that holds it can't leave that thread, even when the output itself is `Send`.
use io_context::AwaitJsLocal;

fn require_send<T: Send>(_: T) {}

fn require_sync<T: Sync>(_: &T) {}

pub fn future(wait: AwaitJsLocal<u32>) {
    require_send(wait); //~ ERROR E0277
}

pub fn shared(wait: &AwaitJsLocal<u32>) {
    require_sync(wait); //~ ERROR E0277
}
