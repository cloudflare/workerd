// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What the garbage collector owns, such as the data of a promise's reaction, holds a span through
// the handle the IoContext gives for it, which is `Send` and `Traced`, as such data must be.
use io_context::IoContext;
use io_context::IoOwnedSpan;

struct Reaction {
    span: IoOwnedSpan,
}

jsg::jsg_traced!(Reaction);

pub fn t(lock: &mut jsg::Lock<'_>) -> jsg::Result<()> {
    let context = IoContext::current(lock)?;
    let span = context.make_trace_span(c"span")?;
    let reaction = Reaction {
        span: context.add_span(span)?,
    };
    reaction
        .span
        .with(lock, |span| span.set_tag(c"tag", true))?;
    Ok(())
}
