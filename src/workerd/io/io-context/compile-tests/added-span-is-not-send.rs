// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A span belongs to its request's thread, so the handle to one that the IoContext owns is not
// `Send` either: it cannot go in the future given to `await_io_and_run`, which is `Send`.
use io_context::IoContext;
use io_context::IoOwned;
use io_context::SpanBuilder;

pub fn t(context: &IoContext<'_>, span: IoOwned<SpanBuilder>) {
    let _ = context.await_io_and_run(async move { drop(span) }, |_lock, ()| Ok(())); //~ ERROR future cannot be sent between threads safely
}
