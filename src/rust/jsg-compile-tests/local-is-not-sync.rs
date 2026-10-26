// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Handles are only valid on the isolate's thread.
//
// This compiles only if the type is not Sync: were it Sync, both impls would apply and the call
// would be ambiguous. A plain `T: Sync` bound would fail with one error per field that is not
// Sync, and how many of those rustc reports differs between versions.
use jsg::v8;

trait AmbiguousIfSync<A> {
    fn some_item() {}
}
impl<T: ?Sized> AmbiguousIfSync<()> for T {}
impl<T: ?Sized + Sync> AmbiguousIfSync<u8> for T {}

pub fn t() {
    let _ = <v8::Local<'static, v8::Value> as AmbiguousIfSync<_>>::some_item;
}
