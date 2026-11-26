// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A channel is an I/O object of the thread it was made on, so a strong reference to one is neither
// `Send` nor `Sync`. That keeps it out of resource state (`Traced: Send`), which can be dropped on
// whichever thread next locks the isolate, and out of the future given to `await_io_and_run`,
// which is `Send` and may be polled without the isolate lock.
//
// This asserts the missing impls rather than failing to compile where a channel crosses threads:
// rustc reports one error for each field of `KjRc` that is not `Send`, and a snippet's
// annotations must not depend on how many errors rustc reports.
use channel_interop::ActorClassChannel;
use channel_interop::ScopedChannel;
use channel_interop::SubrequestChannel;
use static_assertions::assert_not_impl_any;

assert_not_impl_any!(SubrequestChannel: Send, Sync);
assert_not_impl_any!(ActorClassChannel: Send, Sync);
assert_not_impl_any!(ScopedChannel<'static, SubrequestChannel>: Send, Sync);
assert_not_impl_any!(ScopedChannel<'static, ActorClassChannel>: Send, Sync);
