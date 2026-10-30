// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `TracedGlobal` may be dropped on any thread: without the isolate lock, the drop is queued until
// the isolate is locked again. Every other use takes a `Lock` of its isolate.
use jsg::v8;
fn assert_send<T: Send>() {}

pub fn t() {
    assert_send::<v8::TracedGlobal<v8::Value>>();
}
