// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The promise of a C++ `async` function may use the arguments it borrows until it settles, so
// the future, `Send` or not, keeps them borrowed.
use kj_rs::KjFuture;

#[cxx::bridge(namespace = "kj_rs_compile_test")]
mod ffi {
    unsafe extern "C++" {
        include!("kj-rs-compile-test.h");
        async fn write(data: &[u8]);
    }
}

pub fn t() {
    let data = vec![0u8; 4];
    let future = ffi::write(&data).into_send();
    drop(data); //~ ERROR E0505
    drop(future);
}
