// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Frankenvalue's cap table holds I/O objects of the thread whose request made them, shared
// through non-atomic reference counts, so the value is neither `Send` nor `Sync`. That keeps it
// out of resource state (`Traced: Send`), which can be dropped on whichever thread next locks the
// isolate, and out of futures polled without the isolate lock.
//
// This asserts the missing impls rather than failing to compile where a value crosses threads, so
// that the snippet does not depend on how many errors rustc reports.
use frankenvalue::Frankenvalue;
use kj_rs::KjOwn;
use static_assertions::assert_not_impl_any;

assert_not_impl_any!(KjOwn<Frankenvalue>: Send, Sync);
assert_not_impl_any!(Frankenvalue: Send, Sync);
