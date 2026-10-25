// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `enter` borrows the caller's lock for as long as the frame is current, so the closure can only
// use the lock it is handed, not the caller's.
pub fn t(lock: &mut jsg::Lock<'_>, frame: &jsg::AsyncContextFrame) {
    frame.enter(lock, |_inner| lock.new_object()); //~ ERROR E0501

    //~| ERROR E0500
}
