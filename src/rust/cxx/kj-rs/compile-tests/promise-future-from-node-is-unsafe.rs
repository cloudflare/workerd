// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The future owns the promise node it is made from and writes the promise's result as a `T`.
// The compiler can check neither, so the caller vouches for both.
use std::future::Future;

pub fn t(node: &kj_rs::KjPromiseNodeImpl) -> impl Future<Output = Result<u64, cxx::KjException>> {
    kj_rs::new_callbacks_promise_future::<u64>(node) //~ ERROR E0133
}
