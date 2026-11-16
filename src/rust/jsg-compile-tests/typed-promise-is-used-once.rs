// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// As in C++ JSG, a typed promise is used once, since only one reaction can take its value.
pub fn t<'s>(lock: &mut jsg::Lock<'s>, promise: jsg::Promise<'s, String>) -> jsg::Result<()> {
    let _: jsg::Promise<'s, String> = promise.then(lock, (), |_lock, (), s| Ok(s))?;
    let _: jsg::Promise<'s, String> = promise.then(lock, (), |_lock, (), s| Ok(s))?;
    //~^ ERROR E0382
    Ok(())
}
