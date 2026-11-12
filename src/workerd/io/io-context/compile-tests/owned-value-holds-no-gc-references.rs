// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Nothing traces a value an `IoContext` owns, so it holds no JavaScript handles: a
// `TracedGlobal` in it would stay strong, and a cycle through it would never be collected.
use io_context::IoContext;
use jsg::v8;

pub fn traced_global(
    context: &IoContext<'_>,
    value: v8::TracedGlobal<v8::Value>,
) -> jsg::Result<()> {
    let _ = context.add_object(value)?;
    //~^ ERROR E0277
    Ok(())
}

pub fn plain(context: &IoContext<'_>) -> jsg::Result<()> {
    let _ = context.add_object(String::from("plain"))?;
    Ok(())
}
