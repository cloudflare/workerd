// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A slice into V8 memory borrows the lock, so nothing that may run JavaScript (and detach or
// resize the buffer) can be called while it is alive.
use jsg::v8;
pub fn t<'s>(
    lock: &mut jsg::Lock<'s>,
    bytes: v8::Local<'s, v8::Uint8Array>,
    f: v8::Local<'s, v8::Function>,
) {
    let slice = bytes.as_slice(lock);
    let _ = f.call(lock, None, &[]); //~ ERROR E0502
    let _ = slice[0];
}
