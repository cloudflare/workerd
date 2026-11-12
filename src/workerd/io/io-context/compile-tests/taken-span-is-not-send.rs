// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The handle to a span that the IoContext owns is `Send`, but the span taken out of it is not: it
// cannot go in the future given to `await_io_and_run`, which is `Send`.
use io_context::IoContext;
use io_context::IoOwnedSpan;

pub fn t(lock: &mut jsg::Lock<'_>, context: &IoContext<'_>, owned: IoOwnedSpan) {
    let span = owned.take(lock).ok().flatten();
    let _ = context.await_io_and_run(async move { drop(span) }, |_lock, ()| Ok(())); //~ ERROR future cannot be sent between threads safely
}
