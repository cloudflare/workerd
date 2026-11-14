// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::v8;

// The characters are valid only during the call.
pub fn t<'s>(lock: &mut jsg::Lock<'s>, string: v8::Local<'s, v8::String>) {
    let _view = string.with_value_view(lock, |view| view); //~ ERROR lifetime may not live long enough
}
