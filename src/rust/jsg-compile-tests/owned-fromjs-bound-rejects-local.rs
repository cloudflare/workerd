// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `T: for<'s> FromJS<'s, ResultType = T>` (used where a value must outlive the scope) excludes
// handle types.
use jsg::FromJS;
use jsg::v8;

pub fn owned<T: for<'s> FromJS<'s, ResultType = T>>() {}

pub fn t() {
    owned::<v8::Local<'static, v8::Object>>(); //~ ERROR E0277
}
