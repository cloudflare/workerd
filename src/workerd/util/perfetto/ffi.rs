// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to the C++ shims in `ffi.h`, and the safe calls over it.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, the begin calls hand perfetto names it \
              keeps by address, and the category's state is a C++ atomic read in place"
)]

use std::ffi::CStr;
use std::sync::OnceLock;
use std::sync::atomic::AtomicU8;
use std::sync::atomic::Ordering;

#[cxx::bridge(namespace = "workerd::perfetto_bridge")]
mod bridge {
    unsafe extern "C++" {
        include!("workerd/util/perfetto/ffi.h");

        #[nothrow]
        fn category_state() -> *mut u8;

        // `name` is a NUL-terminated string with the NUL, which perfetto keeps by address, so it
        // must live for the rest of the process.
        #[nothrow]
        unsafe fn trace_event_begin(name: &[u8]);
        #[nothrow]
        fn trace_event_end();
        #[nothrow]
        unsafe fn trace_event_begin_on_track(name: &[u8], track: u64);
        #[nothrow]
        fn trace_event_end_on_track(track: u64);
    }
}

/// The category's enabled state, which perfetto keeps in static storage.
#[inline]
fn category_state() -> &'static AtomicU8 {
    static STATE: OnceLock<&'static AtomicU8> = OnceLock::new();
    STATE.get_or_init(|| {
        // SAFETY: `category_state()` returns the address of a `std::atomic<uint8_t>` in static
        // storage, which is never freed and has the layout of an `AtomicU8` (`ffi.c++` asserts
        // it). C++ reads and writes it only atomically, so every access is atomic.
        unsafe { AtomicU8::from_ptr(bridge::category_state()) }
    })
}

/// Whether a tracing session is recording the "workerd" category.
#[inline]
pub fn category_enabled() -> bool {
    // Relaxed, as in perfetto's own `TRACE_EVENT_CATEGORY_ENABLED`.
    category_state().load(Ordering::Relaxed) != 0
}

pub fn begin(name: &'static CStr) {
    // SAFETY: `name` is NUL-terminated and lives for the rest of the process.
    unsafe { bridge::trace_event_begin(name.to_bytes_with_nul()) }
}

pub fn end() {
    bridge::trace_event_end();
}

pub fn begin_on_track(name: &'static CStr, track: u64) {
    // SAFETY: `name` is NUL-terminated and lives for the rest of the process.
    unsafe { bridge::trace_event_begin_on_track(name.to_bytes_with_nul(), track) }
}

pub fn end_on_track(track: u64) {
    bridge::trace_event_end_on_track(track);
}
