// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// An isolate reference stands for holding the isolate lock on this thread; it cannot be shared
// with another one.
//
// This compiles only if the type is not Sync: were it Sync, both impls would apply and the call
// would be ambiguous.
trait AmbiguousIfSync<A> {
    fn some_item() {}
}
impl<T: ?Sized> AmbiguousIfSync<()> for T {}
impl<T: ?Sized + Sync> AmbiguousIfSync<u8> for T {}

pub fn t() {
    let _ = <jsg::Isolate as AmbiguousIfSync<_>>::some_item;
}
