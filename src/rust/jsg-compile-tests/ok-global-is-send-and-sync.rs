// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `Global` may be dropped on any thread, as a `TracedGlobal` may, and shared with other
// threads: GC never writes into it, and every use through a shared reference takes a `Lock` of
// its isolate.
use jsg::v8;
fn assert_send_and_sync<T: Send + Sync>() {}

pub fn t() {
    assert_send_and_sync::<v8::Global<v8::Value>>();
}
