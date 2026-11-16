// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An upcast is a `From`, but a downcast can fail, so it is only a `TryFrom`.
use jsg::v8;

pub fn t(value: v8::Local<'_, v8::Value>) -> v8::Local<'_, v8::Object> {
    value.into() //~ ERROR E0277
}
