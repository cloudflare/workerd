// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The handle to a span that the IoContext owns is `Send`, but the span taken out of it is not.
use io_context::IoOwnedSpan;

fn require_send<T: Send>(_: T) {}

pub fn t(lock: &mut jsg::Lock<'_>, owned: IoOwnedSpan) {
    require_send(owned.take(lock).ok().flatten()); //~ ERROR E0277
}
