// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `Stdin`, the exports of `workerd:stdin`: the process's standard input, and the REPRL loop
//! through which Fuzzilli runs programs.

use std::io::BufRead;

use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

/// The process's standard input, for a build for Fuzzilli.
#[jsg_resource]
pub struct Stdin;

#[jsg_resource]
impl Stdin {
    /// `getline()`: the next line of standard input, without its newline, and up to its first
    /// NUL. It is empty at the end of the input.
    #[jsg_method]
    pub fn getline(&self) -> String {
        let mut line = Vec::new();
        // As `std::getline()`, a failed read ends the line with what was read before it.
        let _ = std::io::stdin().lock().read_until(b'\n', &mut line);
        if line.last() == Some(&b'\n') {
            line.pop();
        }
        String::from_utf8_lossy(up_to_nul(&line)).into_owned()
    }

    /// `reprl()`: runs Fuzzilli's read-eval-print-reset loop, allowing eval. Fuzzilli sends each
    /// program over the REPRL file descriptors, and is told whether it threw. The loop ends only
    /// when a program fails to compile, and then `reprl()` returns.
    #[jsg_method]
    pub fn reprl(&self, lock: &mut jsg::Lock<'_>) {
        lock.set_allow_eval(true);
        reprl::run(lock);
    }
}

/// `bytes` up to its first NUL, as C reads a string.
fn up_to_nul(bytes: &[u8]) -> &[u8] {
    bytes
        .iter()
        .position(|&byte| byte == 0)
        .map_or(bytes, |nul| &bytes[..nul])
}

#[cfg(unix)]
mod reprl {
    #![expect(unsafe_code, reason = "reads and writes Fuzzilli's file descriptors")]

    use crate::ffi::REPRL_COMPILE_FAILED;
    use crate::ffi::bridge;

    /// Fuzzilli's control read, control write and data read file descriptors.
    const CONTROL_READ: libc::c_int = 100;
    const CONTROL_WRITE: libc::c_int = 101;
    const DATA_READ: libc::c_int = 102;

    /// The action that asks to run a program: the bytes `exec`, as a `u32` on a little-endian
    /// machine.
    const EXEC: u32 = 0x6365_7865;

    /// Fails as Fuzzilli's `CHECK()` macro does.
    macro_rules! check {
        ($condition:expr) => {
            if !$condition {
                eprintln!(
                    "Error: {}:{}: condition failed: {}",
                    file!(),
                    line!(),
                    stringify!($condition)
                );
                std::process::exit(libc::EXIT_FAILURE);
            }
        };
    }

    /// One `read(2)` into `buf`; true if it filled it.
    fn read_once(fd: libc::c_int, buf: &mut [u8]) -> bool {
        // SAFETY: `buf` is valid for writes of its length.
        let read = unsafe { libc::read(fd, buf.as_mut_ptr().cast(), buf.len()) };
        usize::try_from(read).is_ok_and(|read| read == buf.len())
    }

    /// One `write(2)` of `buf`; true if it wrote all of it.
    fn write_once(fd: libc::c_int, buf: &[u8]) -> bool {
        // SAFETY: `buf` is valid for reads of its length.
        let written = unsafe { libc::write(fd, buf.as_ptr().cast(), buf.len()) };
        usize::try_from(written).is_ok_and(|written| written == buf.len())
    }

    /// Flushes every C output stream, as `fflush(0)` does.
    fn flush_c_streams() {
        // SAFETY: a null stream flushes all of them.
        unsafe { libc::fflush(std::ptr::null_mut()) };
    }

    pub(super) fn run(lock: &mut jsg::Lock<'_>) {
        let mut helo = *b"HELO";
        if !write_once(CONTROL_WRITE, &helo) || !read_once(CONTROL_READ, &mut helo) {
            println!("Invalid HELO response from parent");
        }
        if helo != *b"HELO" {
            println!("Invalid response from parent");
        }

        loop {
            let mut action = [0; 4];
            let read = read_once(CONTROL_READ, &mut action);
            flush_c_streams();
            let action = u32::from_ne_bytes(action);
            if !read || action != EXEC {
                eprintln!("Unknown action: {action:x}");
                std::process::exit(-1);
            }

            let mut size = [0; 8];
            check!(read_once(CONTROL_READ, &mut size));
            let size = usize::try_from(u64::from_ne_bytes(size)).unwrap_or(usize::MAX);
            let mut script = Vec::new();
            check!(script.try_reserve_exact(size).is_ok());
            script.resize(size, 0);

            let mut loaded = 0;
            while loaded < size {
                let rest = &mut script[loaded..];
                // SAFETY: `rest` is valid for writes of its length.
                let read = unsafe { libc::read(DATA_READ, rest.as_mut_ptr().cast(), rest.len()) };
                let Some(read) = usize::try_from(read).ok().filter(|&read| read > 0) else {
                    eprintln!("Failed to load script");
                    std::process::exit(-1);
                };
                loaded += read;
            }

            // SAFETY: the lock's isolate is locked with a context entered, and the method that
            // runs the loop leaves no exception pending between programs: each runs in a
            // TryCatch that clears what it throws.
            let result = unsafe {
                bridge::reprl_execute(lock.isolate().as_ffi(), super::up_to_nul(&script))
            };
            if result == REPRL_COMPILE_FAILED {
                return;
            }

            flush_c_streams();
            let status = (result & 0xFF) << 8;
            check!(write_once(CONTROL_WRITE, &status.to_ne_bytes()));
            bridge::reprl_reset_edge_guards();
        }
    }
}

#[cfg(not(unix))]
mod reprl {
    /// Fuzzilli runs only on Unix, where its file descriptors are.
    pub(super) fn run(_lock: &mut jsg::Lock<'_>) {}
}
