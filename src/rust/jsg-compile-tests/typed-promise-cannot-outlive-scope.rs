// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A typed promise is a handle in its scope; keeping one beyond it takes a `TracedPromise`.
pub fn t(
    lock: &mut jsg::Lock<'_>,
    kept: &mut Option<jsg::Promise<'static, u32>>,
) -> jsg::Result<()> {
    *kept = Some(jsg::Promise::resolved(lock, 1)?);
    //~^ ERROR lifetime may not live long enough
    Ok(())
}
