// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The lock that `enter` hands its closure is only borrowed for the closure, while the frame is
// current, so it cannot be kept for use after the previous frame is restored.
pub fn t<'s>(lock: &mut jsg::Lock<'s>, frame: &jsg::AsyncContextFrame) {
    let mut escaped = None;
    frame.enter(lock, |lock| escaped = Some(lock)); //~ ERROR E0521
    let _ = escaped.map(|lock: &mut jsg::Lock<'s>| lock.new_object());
}
