// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `await_io`'s continuation gets a fresh lock; its result cannot hold that lock's handles.
pub fn t(lock: &mut jsg::Lock<'_>) {
    let _ = lock.await_io(async {}, |lock, ()| Ok(lock.new_object())); //~ ERROR lifetime may not live long enough
}
