// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A handle that may be empty is tied to the lock's scope like any other.
use jsg::v8;

pub fn t(lock: &mut jsg::Lock<'_>) {
    let _: Option<v8::Local<'static, v8::String>> = v8::String::new_from_utf8(lock, b"x"); //~ ERROR lifetime may not live long enough
}
