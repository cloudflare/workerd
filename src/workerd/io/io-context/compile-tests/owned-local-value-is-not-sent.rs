// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The handle of an `IoOwnedLocal` is `Send`, but the value it hands out stays on the thread that
// takes it: it is as `Send` as it was before the IoContext owned it.
use std::rc::Rc;

use io_context::IoOwnedLocal;

fn send<T: Send>(_value: T) {}

pub fn t(lock: &mut jsg::Lock<'_>, owned: &IoOwnedLocal<Rc<u32>>) -> jsg::Result<()> {
    if let Some(value) = owned.take(lock)? {
        send(value);
        //~^ ERROR E0277
    }
    Ok(())
}
