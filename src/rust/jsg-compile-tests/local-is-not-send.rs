// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Handles are only valid on the isolate's thread. Nor can one leave its scope as a panic payload,
// which must be Send.
//
// This compiles only if the type is not Send: were it Send, both impls would apply and the call
// would be ambiguous. A plain `T: Send` bound would fail with one error per field that is not
// Send, and how many of those rustc reports differs between versions.
use jsg::v8;

trait AmbiguousIfSend<A> {
    fn some_item() {}
}
impl<T: ?Sized> AmbiguousIfSend<()> for T {}
impl<T: ?Sized + Send> AmbiguousIfSend<u8> for T {}

pub fn t() {
    let _ = <v8::Local<'static, v8::Value> as AmbiguousIfSend<_>>::some_item;
}
