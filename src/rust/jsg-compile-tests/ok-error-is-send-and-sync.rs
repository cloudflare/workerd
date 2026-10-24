// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `jsg::Error` may hold what JavaScript threw, in a `Global`, and still crosses threads like
// any other error.
fn assert_send_and_sync<T: Send + Sync>() {}

pub fn t() {
    assert_send_and_sync::<jsg::Error>();
}
