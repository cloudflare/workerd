// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A reentry callback is the C++ callback of `IoContext::makeReentryCallbackWeak()`, which belongs
// to its IoContext's thread: it holds the IoContext's weak reference and a pending event, whose
// counts are not atomic. So neither it nor the future of a call can leave that thread. Since
// `Traced` requires `Send`, it also can't be kept where the garbage collector, which can run on any
// thread that locks the isolate, would drop it.
use io_context::ReentryCallback;

fn require_send<T: Send>(_: T) {}

fn require_sync<T: Sync>(_: &T) {}

pub fn callback(callback: ReentryCallback<u32>) {
    require_send(callback); //~ ERROR E0277
}

pub fn shared(callback: &ReentryCallback<u32>) {
    require_sync(callback); //~ ERROR E0277
}

pub fn call(callback: &ReentryCallback<u32>) {
    require_send(callback.call()); //~ ERROR E0277

    //~| ERROR E0277
}
