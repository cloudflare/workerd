// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Perfetto trace events from Rust, the counterpart of C++ `TRACE_EVENT("workerd", "Name")`
//! (`workerd/util/perfetto-tracing.h`).
//!
//! Events go to the "workerd" category. A slice is open from the macro to the end of the
//! enclosing block:
//!
//! ```ignore
//! fn parse() {
//!     perfetto::trace_event!("Parser::parse()");
//!     // ...
//! }
//!
//! async fn request() {
//!     perfetto::trace_event_async!("HttpConnection::request()");
//!     worker.request().await;
//! }
//! ```
//!
//! # Which track
//!
//! [`trace_event!`] opens a slice on the current thread's track, as C++ `TRACE_EVENT` does.
//! Slices on a thread track are a stack: an end event carries no name, and the trace processor
//! closes the innermost open slice of the track with it. So a thread-track slice must not be open
//! across an `.await`. Futures that share a thread (workerd's event loop, a tokio `LocalSet`) run
//! in turns, and when one yields with a slice open, another can open its own on the same track;
//! the first future's end then closes the second future's slice, and both slices get the wrong
//! extent, silently. [`ScopedSlice`] is therefore `!Send`, and `clippy.toml` makes holding it
//! across an `.await` a lint error (`await_holding_invalid_type`).
//!
//! [`trace_event_async!`] opens a slice on a track of its own, a child of the process track, so
//! other work running on the thread in between does not disturb it, and it may end on another
//! thread. Each slice is a separate track in the trace, as with C++
//! `TRACE_EVENT_BEGIN(..., PERFETTO_TRACK_FROM_POINTER(p))`, so while tracing, every async slice
//! adds a track descriptor to the trace and a track row to the UI.
//!
//! # Cost when not tracing
//!
//! Creating a slice reads the category's enabled state, a byte that perfetto keeps in static
//! storage, with a relaxed atomic load from Rust: the first call in the process asks C++ for its
//! address, and later calls make no FFI call, only a check that the address is cached. A slice that began while the category was disabled
//! makes no call when it ends either. With perfetto compiled out, the state is a byte that is
//! never set.
//!
//! # Names
//!
//! Names are `&'static CStr`s: perfetto interns a static name by its address and reads it when it
//! first writes the name to a trace, so it must be NUL-terminated and live for the rest of the
//! process. The macros take a string literal and make it one at compile time.

mod ffi;

use std::ffi::CStr;
use std::marker::PhantomData;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;

pub use ffi::category_enabled;

/// A slice on the current thread's track, until the guard is dropped.
///
/// Must not be held across an `.await`; see the crate documentation. Guards on a thread must be
/// dropped in the reverse order of their creation, as scoped locals are: an end event closes the
/// innermost open slice, whichever guard ends it.
#[must_use = "the slice ends when the guard is dropped"]
pub struct ScopedSlice {
    active: bool,
    /// The slice must end on the thread whose track it began on.
    _not_send: PhantomData<*const ()>,
}

impl ScopedSlice {
    /// Begins a slice named `name`, if the category is enabled.
    #[inline]
    pub fn begin(name: &'static CStr) -> Self {
        let active = category_enabled();
        if active {
            ffi::begin(name);
        }
        Self {
            active,
            _not_send: PhantomData,
        }
    }
}

impl Drop for ScopedSlice {
    #[inline]
    fn drop(&mut self) {
        // A slice that began while the category was disabled has no begin event to end, even if
        // the category has been enabled since.
        if self.active {
            ffi::end();
        }
    }
}

/// A slice on a track of its own, from [`AsyncSlice::begin`] until the guard is dropped. It may
/// be held across an `.await` and dropped on any thread.
#[must_use = "the slice ends when the guard is dropped"]
pub struct AsyncSlice {
    /// The slice's track, or `None` if the category was disabled when it began.
    track: Option<u64>,
}

impl AsyncSlice {
    /// Begins a slice named `name` on a new track, if the category is enabled.
    #[inline]
    pub fn begin(name: &'static CStr) -> Self {
        let track = category_enabled().then(|| {
            let track = next_track();
            ffi::begin_on_track(name, track);
            track
        });
        Self { track }
    }
}

impl Drop for AsyncSlice {
    #[inline]
    fn drop(&mut self) {
        if let Some(track) = self.track {
            ffi::end_on_track(track);
        }
    }
}

/// A track id that no other slice of this process uses. Ids are hashes rather than small
/// integers, so that they don't collide with the ids of other tracks in the trace, as in
/// `workerd/io/async-trace-perfetto.c++`.
fn next_track() -> u64 {
    static NEXT: AtomicU64 = AtomicU64::new(0);
    // A salt, and the splitmix64 finalizer, which is a bijection, so distinct counts give
    // distinct ids.
    const SALT: u64 = 0x7275_7374_5f73_6c31; // "rust_sl1"
    let mut x = NEXT.fetch_add(1, Ordering::Relaxed) ^ SALT;
    x = (x ^ (x >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    x = (x ^ (x >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    x ^ (x >> 31)
}

/// Makes a string literal into the `&'static CStr` a trace event name is, at compile time.
#[doc(hidden)]
#[expect(
    clippy::panic,
    reason = "evaluated at compile time, where a panic is a compile error"
)]
pub const fn __event_name(name_with_nul: &'static str) -> &'static CStr {
    match CStr::from_bytes_with_nul(name_with_nul.as_bytes()) {
        Ok(name) => name,
        Err(_) => panic!("a trace event name cannot contain a NUL"),
    }
}

/// C++ `TRACE_EVENT("workerd", name)`: a slice on the current thread's track.
///
/// The slice, named by a string literal, is open until the end of the enclosing block. Not for a
/// block that `.await`s; see [`trace_event_async!`].
#[macro_export]
macro_rules! trace_event {
    ($name:literal) => {
        let _perfetto_slice = $crate::ScopedSlice::begin(
            const { $crate::__event_name(::core::concat!($name, "\0")) },
        );
    };
}

/// A slice on a track of its own, which may span an `.await`.
///
/// The slice, named by a string literal, is open until the end of the enclosing block.
#[macro_export]
macro_rules! trace_event_async {
    ($name:literal) => {
        let _perfetto_slice =
            $crate::AsyncSlice::begin(const { $crate::__event_name(::core::concat!($name, "\0")) });
    };
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
