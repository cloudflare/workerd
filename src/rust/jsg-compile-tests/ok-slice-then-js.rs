// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A slice ends before JavaScript runs.
use jsg::v8;
pub fn t<'s>(
    lock: &mut jsg::Lock<'s>,
    bytes: v8::Local<'s, v8::Uint8Array>,
    f: v8::Local<'s, v8::Function>,
) -> Result<u8, jsg::Error> {
    let first = bytes.as_slice(lock)[0];
    f.call(lock, None, &[])?;
    Ok(first)
}
