// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The future a C++ `async` function returns belongs to the event loop that made its promise, so
// it is not `Send` until `into_send()` makes it so.
#[cxx::bridge(namespace = "kj_rs_compile_test")]
mod ffi {
    unsafe extern "C++" {
        include!("kj-rs-compile-test.h");
        async fn promise() -> i32;
    }
}

fn require_send<T: Send>(_: T) {}

pub fn t() {
    require_send(ffi::promise()); //~ ERROR cannot be sent between threads safely
}
