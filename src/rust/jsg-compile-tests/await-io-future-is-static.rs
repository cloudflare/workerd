// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The future given to `await_io`, and the continuation that runs after it, run after the
// callback has returned, so neither can borrow from it.
pub fn future(lock: &mut jsg::Lock<'_>, data: &[u8]) {
    let _ = lock.await_io(async { data.len() }, |_lock, _len| Ok(())); //~ ERROR E0521
}

pub fn continuation(lock: &mut jsg::Lock<'_>, data: &[u8]) {
    let _ = lock.await_io(async {}, |_lock, ()| Ok(data.len())); //~ ERROR E0521
}
