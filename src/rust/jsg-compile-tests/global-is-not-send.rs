// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `Global` is only usable (and droppable) on the isolate's thread.
use jsg::v8;
fn assert_send<T: Send>() {}

pub fn t() {
    assert_send::<v8::Global<v8::Value>>(); //~ ERROR E0277
}
