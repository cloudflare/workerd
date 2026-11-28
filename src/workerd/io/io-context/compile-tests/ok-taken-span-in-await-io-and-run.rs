// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A span belongs to its request's event loop, as the future and the callback given to
// `await_io_and_run` do, so both can hold one, even one taken out of the IoContext.
use io_context::IoContext;
use io_context::IoOwnedSpan;

pub fn t(lock: &mut jsg::Lock<'_>, context: &IoContext<'_>, owned: IoOwnedSpan) -> jsg::Result<()> {
    let taken = owned.take(lock)?;
    let mut span = context.make_trace_span(c"span")?;
    context.await_io_and_run(async move { drop(taken) }, move |_lock, ()| {
        span.set_tag(c"done", true);
        Ok(())
    })
}
