// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Polling the future of a C++ promise links the awaiter inside it into the event loop, so the
// future must stay where it was polled: it is not `Unpin`.
#[cxx::bridge(namespace = "kj_rs_compile_test")]
mod ffi {
    unsafe extern "C++" {
        include!("kj-rs-compile-test.h");
        async fn promise() -> i32;
    }
}

fn require_unpin<T: Unpin>(_: &T) {}

pub fn t() {
    require_unpin(&ffi::promise()); //~ ERROR E0277
}
