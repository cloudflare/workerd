// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::UnsafeCell;
use std::hint::black_box;
use std::sync::Arc;
use std::sync::Barrier;
use std::thread;

struct Shared(UnsafeCell<u64>);

// This binary deliberately violates Rust's synchronization requirements to
// verify the sanitizer configuration, so its shared cell must bypass Send/Sync.
unsafe impl Send for Shared {}
unsafe impl Sync for Shared {}

fn main() {
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
}
