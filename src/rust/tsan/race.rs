// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::hint::black_box;
use std::sync::Arc;
use std::sync::Barrier;
use std::thread;

struct Shared(UnsafeCell<u64>);

// This binary deliberately violates Rust's synchronization requirements to
// verify the sanitizer configuration, so its shared cell must bypass Send/Sync.
unsafe impl Send for Shared {}
unsafe impl Sync for Shared {}

unsafe extern "C" {
    fn _exit(status: i32) -> !;
}

// TSan invokes this hook before applying halt_on_error. Reaching it proves that the concurrent
// writes below were instrumented. Use _exit() because process::exit() can deadlock inside TSan.
#[unsafe(no_mangle)]
extern "C" fn __tsan_on_report(_: *mut c_void) {
    // SAFETY: _exit() terminates the process immediately and does not return to the caller.
    unsafe { _exit(0) }
}

#[test]
fn detects_data_race() {
    let value = Arc::new(Shared(UnsafeCell::new(0)));
    let barrier = Arc::new(Barrier::new(3));
    let mut threads = Vec::new();

    for thread_value in [1, 2] {
        let value = Arc::clone(&value);
        let barrier = Arc::clone(&barrier);
        threads.push(thread::spawn(move || {
            barrier.wait();
            for _ in 0..100_000 {
                // SAFETY: This unsynchronized access is intentional test input
                // for ThreadSanitizer and must never be used as an example.
                unsafe { value.0.get().write_volatile(thread_value) };
            }
        }));
    }

    barrier.wait();
    for thread in threads {
        thread.join().unwrap();
    }
    // Keep the shared memory live and observable until both threads complete.
    black_box(value);

    panic!("ThreadSanitizer did not detect the concurrent writes");
}
