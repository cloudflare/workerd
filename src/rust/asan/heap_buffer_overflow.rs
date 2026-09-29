// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::hint::black_box;
use std::process::Command;

const CHILD_ENV: &str = "WORKERD_ASAN_TEST_CHILD";

// The out-of-bounds read runs in a child process: ASan aborts after reporting, and weak hooks such
// as __asan_on_error cannot be overridden from the executable when the runtime is a dylib (macOS).
#[test]
fn detects_heap_buffer_overflow() {
    if std::env::var_os(CHILD_ENV).is_some() {
        let xs = black_box(vec![0u8; 4]);
        // SAFETY: This out-of-bounds read is intentional test input for AddressSanitizer and must
        // never be used as an example.
        let past_end = unsafe { *xs.as_ptr().add(4) };
        black_box(past_end);
        return;
    }

    let output = Command::new(std::env::current_exe().expect("test executable path"))
        .args(["--exact", "detects_heap_buffer_overflow"])
        .env(CHILD_ENV, "1")
        .output()
        .expect("spawn child test process");
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        !output.status.success()
            && stderr.contains("ERROR: AddressSanitizer: heap-buffer-overflow"),
        "AddressSanitizer did not detect the out-of-bounds read in Rust code: {:?}\n{stderr}",
        output.status,
    );
}
