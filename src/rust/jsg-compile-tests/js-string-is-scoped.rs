// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::FromJS;
use jsg::JsString;
use jsg::v8;

pub fn t<'s>(lock: &mut jsg::Lock<'s>, value: v8::Local<'s, v8::Value>) {
    let _: JsString<'static> = JsString::from_js(lock, value).unwrap(); //~ ERROR lifetime may not live long enough
}
