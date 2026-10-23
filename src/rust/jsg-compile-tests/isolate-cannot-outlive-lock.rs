// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An isolate borrowed from a lock proves the isolate is locked only while the lock is borrowed.
pub fn t<'a>(lock: &'a mut jsg::Lock<'static>) -> &'static mut jsg::Isolate {
    lock
    //~^ ERROR lifetime may not live long enough
}
