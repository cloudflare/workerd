// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A typed promise's reaction captures nothing, as an untyped one's does: its state goes in the
// reaction's data, which the garbage collector can see.
pub fn t<'s>(
    lock: &mut jsg::Lock<'s>,
    promise: jsg::Promise<'s, u32>,
) -> jsg::Result<jsg::Promise<'s, String>> {
    let prefix = String::from("captured");
    promise.then(lock, (), move |_lock, (), n| Ok(format!("{prefix}{n}")))
    //~^ ERROR E0277
}
