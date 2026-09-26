//! Perfetto trace points for Rust code in workerd.
//!
//! Rust trace points are written into the same Perfetto session as workerd's C++ `TRACE_EVENT`s
//! and V8's events, so they appear on the same timeline in one trace file. The macros mirror the
//! C++ ones from `src/workerd/util/perfetto-tracing.h`:
//!
//! | C++                                  | Rust                                           |
//! | ------------------------------------ | ---------------------------------------------- |
//! | `TRACE_EVENT`                        | [`trace_event!`]                               |
//! | `TRACE_EVENT_BEGIN`                  | [`trace_event_begin!`]                         |
//! | `TRACE_EVENT_END`                    | [`trace_event_end!`]                           |
//! | `TRACE_EVENT_INSTANT`                | [`trace_event_instant!`]                       |
//! | `TRACE_COUNTER`                      | [`trace_counter!`]                             |
//! | `TRACE_EVENT_CATEGORY_ENABLED`       | [`trace_event_category_enabled!`]              |
//! | `PERFETTO_FLOW_FROM_POINTER`         | [`Flow::from_ref`], [`Flow::from_ptr`]         |
//! | `PERFETTO_TERMINATING_FLOW_FROM_POINTER` | [`EventContext::set_terminating_flow`]     |
//! | `PERFETTO_TRACK_FROM_POINTER`        | [`Track::from_ref`], [`Track::from_ptr`]       |
//!
//! Arguments go in a closure instead of trailing macro arguments:
//! `trace_event!("workerd", "Foo::bar()", |ctx| { ctx.add_arg("id", id); })`.
//!
//! ```ignore
//! use perfetto::{trace_event, Flow};
//!
//! fn handle(&self, url: &str) {
//!     trace_event!("workerd", "Handler::handle()", |ctx| {
//!         ctx.add_arg("url", url).set_flow(Flow::from_ref(self));
//!     });
//!     // ...
//! }
//! ```
//!
//! Categories are string literals checked at compile time against [`CATEGORIES`]. Event names are
//! string literals too. The closure that adds arguments only runs when the category is enabled in
//! the active session, so it must not have side effects beyond filling in the [`EventContext`].
//!
//! Flow and track IDs are computed the same way as in the C++ SDK, so a flow started in C++ with
//! `PERFETTO_FLOW_FROM_POINTER(p)` connects to `Flow::from_ref(p)` in Rust, and begin/end events on
//! `PERFETTO_TRACK_FROM_POINTER(p)` and `Track::from_ref(p)` land on the same track.
//!
//! # Build configurations
//!
//! The Perfetto backend is compiled in only when the `perfetto` feature is enabled, which Bazel
//! does under `//src/workerd/util:really_use_rust_perfetto`. Without it every macro still
//! type-checks its arguments but compiles to nothing, and this crate has no Perfetto dependency.
//! Code using these macros must build in both configurations; the public types are identical in
//! both, including their auto traits.
//!
//! # Cost
//!
//! A trace point whose category is disabled costs one atomic load. An enabled one is more
//! expensive than its C++ counterpart: the Perfetto Rust SDK takes a process-wide mutex for every
//! emitted event (its generated category table is a `static mut` that it guards with a lock) and
//! allocates for the event's extras. Keep that in mind before adding trace points to hot paths.
//!
//! # Async code
//!
//! A [`trace_event!`] slice is tied to the enclosing scope on the current thread track. Don't hold
//! one across an `.await`: other tasks run on the same thread in the meantime and the slices no
//! longer nest, and the task may resume on another thread. [`ScopedEvent`] isn't `Send`, so a
//! `Send` future that holds one across an `.await` doesn't compile. For work that awaits, emit [`trace_event_begin!`] / [`trace_event_end!`] on a
//! [`Track`] derived from an object that lives for the duration of the work.
//!
//! # Initialization
//!
//! The C++ side owns Perfetto initialization. `workerd::PerfettoSession::registerWorkerdTracks()`
//! calls into this crate to register the Rust categories after `perfetto::Tracing::Initialize()`.
//! Until then all Rust categories read as disabled.

// Production code must not panic; test code is exempt via clippy.toml allow-*-in-tests.
#![deny(clippy::expect_used, clippy::panic, clippy::unreachable)]
#![deny(clippy::todo, clippy::unimplemented)]

use std::ffi::CStr;
use std::marker::PhantomData;

#[cfg(not(feature = "perfetto"))]
mod disabled;
#[cfg(feature = "perfetto")]
mod enabled;
mod ffi;

#[cfg(not(feature = "perfetto"))]
use disabled as backend;
#[cfg(feature = "perfetto")]
use enabled as backend;

/// Defines the Rust track event categories. Each entry is `(name, description)`.
///
/// The Perfetto SDK's category table is generated from the same list, so the index of a category
/// in [`CATEGORIES`] is also its index in the SDK.
macro_rules! define_categories {
    ($(($name:literal, $description:literal)),+ $(,)?) => {
        /// Names of the track event categories that Rust code can emit into.
        pub const CATEGORIES: &[&str] = &[$($name),+];

        #[cfg(feature = "perfetto")]
        #[expect(
            clippy::significant_drop_tightening,
            reason = "in the Perfetto SDK's generated code"
        )]
        mod sdk {
            perfetto_sdk::track_event_categories! {
                pub(crate) mod categories {
                    $(($name, $description, [])),+
                }
            }
        }
    };
}

define_categories! {
    // Shares its name with the C++ `workerd` category, so enabling `workerd` in a trace config
    // enables trace points in both languages. This keeps a module's trace points under the same
    // category when it's ported from C++ to Rust.
    ("workerd", "workerd runtime events"),
}

/// Whether this build includes the Perfetto backend. When `false`, all trace points are no-ops.
pub const ENABLED_IN_BUILD: bool = cfg!(feature = "perfetto");

/// Registers the Rust track event categories with Perfetto.
///
/// Called by C++ (`PerfettoSession::registerWorkerdTracks()`) after Perfetto has been initialized.
/// Calling it more than once is harmless.
pub fn register_track_events() {
    backend::register_track_events();
}

/// A debug annotation value attached to an event with [`EventContext::add_arg`].
#[derive(Debug, Clone, Copy)]
pub enum DebugArg<'a> {
    Bool(bool),
    Int(i64),
    Uint(u64),
    Double(f64),
    /// Strings are truncated at the first NUL byte.
    Str(&'a str),
    Pointer(usize),
}

macro_rules! debug_arg_from {
    ($variant:ident($target:ty): $($source:ty),+) => {
        $(
            impl From<$source> for DebugArg<'_> {
                fn from(value: $source) -> Self {
                    Self::$variant(<$target>::from(value))
                }
            }
        )+
    };
}

debug_arg_from!(Bool(bool): bool);
debug_arg_from!(Int(i64): i8, i16, i32, i64);
debug_arg_from!(Uint(u64): u8, u16, u32, u64);
debug_arg_from!(Double(f64): f32, f64);

impl From<usize> for DebugArg<'_> {
    fn from(value: usize) -> Self {
        // usize is at most 64 bits on every supported target.
        Self::Uint(value as u64)
    }
}

impl From<isize> for DebugArg<'_> {
    fn from(value: isize) -> Self {
        // isize is at most 64 bits on every supported target.
        Self::Int(value as i64)
    }
}

impl<'a> From<&'a str> for DebugArg<'a> {
    fn from(value: &'a str) -> Self {
        Self::Str(value)
    }
}

impl<'a> From<&'a String> for DebugArg<'a> {
    fn from(value: &'a String) -> Self {
        Self::Str(value.as_str())
    }
}

/// A value recorded with [`trace_counter!`].
#[derive(Debug, Clone, Copy)]
pub enum CounterValue {
    Int(i64),
    Double(f64),
}

macro_rules! counter_value_from {
    ($variant:ident($target:ty): $($source:ty),+) => {
        $(
            impl From<$source> for CounterValue {
                fn from(value: $source) -> Self {
                    Self::$variant(<$target>::from(value))
                }
            }
        )+
    };
}

counter_value_from!(Int(i64): i8, i16, i32, i64, u8, u16, u32);
counter_value_from!(Double(f64): f32, f64);

/// Connects events across threads and across the C++/Rust boundary.
///
/// Attach it to the events to connect with [`EventContext::set_flow`], and to the last one with
/// [`EventContext::set_terminating_flow`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Flow {
    id: u64,
    process_scoped: bool,
}

impl Flow {
    /// A flow identified by an object's address. Equivalent to C++
    /// `PERFETTO_FLOW_FROM_POINTER(&object)`.
    ///
    /// The address can be reused once the object is freed, so terminate the flow before that.
    #[must_use]
    pub fn from_ref<T: ?Sized>(object: &T) -> Self {
        Self::from_ptr(std::ptr::from_ref(object))
    }

    /// A flow identified by an address. Equivalent to C++ `PERFETTO_FLOW_FROM_POINTER(ptr)`.
    #[must_use]
    pub fn from_ptr<T: ?Sized>(ptr: *const T) -> Self {
        Self::process_scoped(address(ptr))
    }

    /// A flow identified by an ID that is unique within this process. Equivalent to C++
    /// `perfetto::Flow::ProcessScoped(id)`.
    #[must_use]
    pub const fn process_scoped(id: u64) -> Self {
        Self {
            id,
            process_scoped: true,
        }
    }

    /// A flow identified by a globally unique ID. Equivalent to C++ `perfetto::Flow::Global(id)`.
    #[must_use]
    pub const fn global(id: u64) -> Self {
        Self {
            id,
            process_scoped: false,
        }
    }

    /// The flow ID as written to the trace. Process-scoped IDs incorporate the process track
    /// UUID, which is only known once Perfetto is initialized; it's 0 in builds without Perfetto.
    #[must_use]
    pub fn id(&self) -> u64 {
        if self.process_scoped {
            self.id ^ backend::process_track_uuid()
        } else {
            self.id
        }
    }
}

/// An anonymous track, child of the process track, for begin/end events that don't belong on the
/// current thread's track (typically async work).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Track {
    id: u64,
}

impl Track {
    /// A track identified by an object's address. Equivalent to C++
    /// `PERFETTO_TRACK_FROM_POINTER(&object)`.
    #[must_use]
    pub fn from_ref<T: ?Sized>(object: &T) -> Self {
        Self::from_ptr(std::ptr::from_ref(object))
    }

    /// A track identified by an address. Equivalent to C++ `PERFETTO_TRACK_FROM_POINTER(ptr)`.
    #[must_use]
    pub fn from_ptr<T: ?Sized>(ptr: *const T) -> Self {
        Self::new(address(ptr))
    }

    /// A track identified by an ID that is unique within this process. Equivalent to C++
    /// `perfetto::Track(id)`.
    #[must_use]
    pub const fn new(id: u64) -> Self {
        Self { id }
    }

    /// The track UUID as written to the trace. It incorporates the process track UUID, which is
    /// only known once Perfetto is initialized; it's 0 in builds without Perfetto.
    #[must_use]
    pub fn uuid(&self) -> u64 {
        self.id ^ backend::process_track_uuid()
    }
}

/// Extra data for an event. Passed to the closure given to the trace macros, which only runs when
/// the event's category is enabled.
pub struct EventContext<'a> {
    inner: backend::EventContext<'a>,
    // Not Send or Sync in either build configuration.
    _not_send: PhantomData<*mut ()>,
}

impl EventContext<'_> {
    /// Adds a debug annotation. Names are truncated at the first NUL byte.
    pub fn add_arg<'v>(&mut self, name: &str, value: impl Into<DebugArg<'v>>) -> &mut Self {
        self.inner.add_arg(truncate_at_nul(name), value.into());
        self
    }

    /// Puts the event on `track` instead of the current thread's track. Begin and end events of a
    /// slice must use the same track.
    pub fn set_track(&mut self, track: Track) -> &mut Self {
        self.inner.set_track(track);
        self
    }

    /// Connects the event to other events with the same flow.
    pub fn set_flow(&mut self, flow: Flow) -> &mut Self {
        self.inner.set_flow(flow);
        self
    }

    /// Connects the event to other events with the same flow and ends the flow.
    pub fn set_terminating_flow(&mut self, flow: Flow) -> &mut Self {
        self.inner.set_terminating_flow(flow);
        self
    }
}

/// Ends the slice started by [`trace_event!`] when dropped.
///
/// Not `Send`: the end event goes on the track of the thread that drops it, which must be the
/// thread where the slice began.
#[must_use]
pub struct ScopedEvent {
    category: usize,
    // Whether the begin event was emitted; the end event is only emitted if so.
    active: bool,
    // A scoped event must be dropped on the thread where its slice began.
    _not_send: PhantomData<*mut ()>,
}

impl Drop for ScopedEvent {
    fn drop(&mut self) {
        if self.active {
            backend::emit_end(self.category, |_| {});
        }
    }
}

// Code using the facade must compile in both build configurations, so the public types must have
// the same auto traits in both. These assertions are checked in both configurations.
static_assertions::assert_impl_all!(Flow: Send, Sync, Unpin);
static_assertions::assert_impl_all!(Track: Send, Sync, Unpin);
static_assertions::assert_impl_all!(ScopedEvent: Unpin);
static_assertions::assert_not_impl_any!(ScopedEvent: Send, Sync);
static_assertions::assert_impl_all!(DebugArg<'static>: Send, Sync, Unpin);
static_assertions::assert_impl_all!(CounterValue: Send, Sync, Unpin);
static_assertions::assert_not_impl_any!(EventContext<'static>: Send, Sync);

fn address<T: ?Sized>(ptr: *const T) -> u64 {
    // Addresses are at most 64 bits on every supported target.
    ptr.cast::<()>().addr() as u64
}

fn truncate_at_nul(s: &str) -> &str {
    s.find('\0').map_or(s, |end| &s[..end])
}

/// Implementation details of the macros. Not part of the public API.
#[doc(hidden)]
pub mod __private {
    use super::CATEGORIES;
    use super::CStr;
    use super::CounterValue;
    use super::EventContext;
    use super::PhantomData;
    use super::ScopedEvent;
    use super::backend;

    /// Returns the index of `category` in [`CATEGORIES`]. Evaluated at compile time by the macros,
    /// so an unknown category is a compile error.
    #[expect(
        clippy::panic,
        reason = "only evaluated in const contexts, where a panic is a compile error"
    )]
    #[must_use]
    pub const fn category_index(category: &str) -> usize {
        let mut i = 0;
        while i < CATEGORIES.len() {
            if str_eq(CATEGORIES[i], category) {
                return i;
            }
            i += 1;
        }
        panic!("unknown Perfetto category; add it to perfetto::CATEGORIES")
    }

    /// Converts a NUL-terminated string literal into a `CStr`. Evaluated at compile time by the
    /// macros, so an event name with an interior NUL is a compile error.
    #[expect(
        clippy::panic,
        reason = "only evaluated in const contexts, where a panic is a compile error"
    )]
    #[must_use]
    pub const fn event_name(with_nul: &'static str) -> &'static CStr {
        match CStr::from_bytes_with_nul(with_nul.as_bytes()) {
            Ok(name) => name,
            Err(_) => panic!("Perfetto event names must not contain NUL"),
        }
    }

    const fn str_eq(a: &str, b: &str) -> bool {
        let a = a.as_bytes();
        let b = b.as_bytes();
        if a.len() != b.len() {
            return false;
        }
        let mut i = 0;
        while i < a.len() {
            if a[i] != b[i] {
                return false;
            }
            i += 1;
        }
        true
    }

    fn wrap(inner: backend::EventContext<'_>) -> EventContext<'_> {
        EventContext {
            inner,
            _not_send: PhantomData,
        }
    }

    // Each of these takes the closure through a generic `FnOnce(&mut EventContext)` bound so that
    // closure parameter types are inferred at the call site.

    #[inline]
    #[must_use]
    pub fn is_enabled(category: usize) -> bool {
        backend::is_enabled(category)
    }

    #[inline]
    pub fn instant(category: usize, name: &'static CStr, f: impl FnOnce(&mut EventContext<'_>)) {
        backend::emit_instant(category, name, |ctx| f(&mut wrap(ctx)));
    }

    #[inline]
    pub fn begin(category: usize, name: &'static CStr, f: impl FnOnce(&mut EventContext<'_>)) {
        backend::emit_begin(category, name, |ctx| f(&mut wrap(ctx)));
    }

    #[inline]
    pub fn end(category: usize, f: impl FnOnce(&mut EventContext<'_>)) {
        backend::emit_end(category, |ctx| f(&mut wrap(ctx)));
    }

    #[inline]
    pub fn scoped(
        category: usize,
        name: &'static CStr,
        f: impl FnOnce(&mut EventContext<'_>),
    ) -> ScopedEvent {
        let active = backend::emit_begin(category, name, |ctx| f(&mut wrap(ctx)));
        ScopedEvent {
            category,
            active,
            _not_send: PhantomData,
        }
    }

    #[inline]
    pub fn counter(category: usize, name: &'static str, value: impl FnOnce() -> CounterValue) {
        backend::emit_counter(category, name, value);
    }
}

/// Emits a slice covering the rest of the enclosing scope. Equivalent to C++ `TRACE_EVENT`.
///
/// ```ignore
/// trace_event!("workerd", "Foo::bar()");
/// trace_event!("workerd", "Foo::bar()", |ctx| { ctx.add_arg("id", id); });
/// ```
#[macro_export]
macro_rules! trace_event {
    ($category:literal, $name:literal $(,)?) => {
        $crate::trace_event!($category, $name, |_| {})
    };
    ($category:literal, $name:literal, $f:expr $(,)?) => {
        let _workerd_perfetto_scoped_event = $crate::__private::scoped(
            {
                const CATEGORY: usize = $crate::__private::category_index($category);
                CATEGORY
            },
            {
                const NAME: &::core::ffi::CStr =
                    $crate::__private::event_name(::core::concat!($name, "\0"));
                NAME
            },
            $f,
        );
    };
}

/// Begins a slice. End it with [`trace_event_end!`] on the same track. Equivalent to C++
/// `TRACE_EVENT_BEGIN`.
#[macro_export]
macro_rules! trace_event_begin {
    ($category:literal, $name:literal $(,)?) => {
        $crate::trace_event_begin!($category, $name, |_| {})
    };
    ($category:literal, $name:literal, $f:expr $(,)?) => {
        $crate::__private::begin(
            {
                const CATEGORY: usize = $crate::__private::category_index($category);
                CATEGORY
            },
            {
                const NAME: &::core::ffi::CStr =
                    $crate::__private::event_name(::core::concat!($name, "\0"));
                NAME
            },
            $f,
        )
    };
}

/// Ends the most recent slice begun with [`trace_event_begin!`] on the same track. Equivalent to
/// C++ `TRACE_EVENT_END`.
#[macro_export]
macro_rules! trace_event_end {
    ($category:literal $(,)?) => {
        $crate::trace_event_end!($category, |_| {})
    };
    ($category:literal, $f:expr $(,)?) => {
        $crate::__private::end(
            {
                const CATEGORY: usize = $crate::__private::category_index($category);
                CATEGORY
            },
            $f,
        )
    };
}

/// Emits an instant event. Equivalent to C++ `TRACE_EVENT_INSTANT`.
#[macro_export]
macro_rules! trace_event_instant {
    ($category:literal, $name:literal $(,)?) => {
        $crate::trace_event_instant!($category, $name, |_| {})
    };
    ($category:literal, $name:literal, $f:expr $(,)?) => {
        $crate::__private::instant(
            {
                const CATEGORY: usize = $crate::__private::category_index($category);
                CATEGORY
            },
            {
                const NAME: &::core::ffi::CStr =
                    $crate::__private::event_name(::core::concat!($name, "\0"));
                NAME
            },
            $f,
        )
    };
}

/// Records a counter value on a process-wide counter track named `name`.
///
/// Equivalent to C++ `TRACE_COUNTER(category, name, value)`, and uses the same track as a C++
/// counter with the same name. `value` is only evaluated when the category is enabled.
#[macro_export]
macro_rules! trace_counter {
    ($category:literal, $name:literal, $value:expr $(,)?) => {
        $crate::__private::counter(
            {
                const CATEGORY: usize = $crate::__private::category_index($category);
                CATEGORY
            },
            $name,
            || $crate::CounterValue::from($value),
        )
    };
}

/// Whether `category` is enabled in an active tracing session. Equivalent to C++
/// `TRACE_EVENT_CATEGORY_ENABLED`. Always `false` in builds without Perfetto.
#[macro_export]
macro_rules! trace_event_category_enabled {
    ($category:literal $(,)?) => {
        $crate::__private::is_enabled({
            const CATEGORY: usize = $crate::__private::category_index($category);
            CATEGORY
        })
    };
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn category_index_is_const() {
        const WORKERD: usize = __private::category_index("workerd");
        assert_eq!(CATEGORIES[WORKERD], "workerd");
    }

    #[test]
    fn truncates_at_nul() {
        assert_eq!(truncate_at_nul("abc"), "abc");
        assert_eq!(truncate_at_nul("a\0bc"), "a");
        assert_eq!(truncate_at_nul(""), "");
    }

    // Trace points must compile and be callable in both build configurations, whether or not
    // Perfetto has been initialized.
    #[test]
    fn macros_compile_without_session() {
        let value = 42i64;
        let name = String::from("name");
        trace_event!("workerd", "scoped");
        trace_event!("workerd", "scoped with args", |ctx| {
            ctx.add_arg("value", value)
                .add_arg("name", &name)
                .add_arg("flag", true)
                .add_arg("ratio", 0.5)
                .set_flow(Flow::from_ref(&value));
        });
        trace_event_begin!("workerd", "begin", |ctx| {
            ctx.set_track(Track::from_ref(&value));
        });
        trace_event_end!("workerd", |ctx| {
            ctx.set_track(Track::from_ref(&value))
                .set_terminating_flow(Flow::global(1));
        });
        trace_event_instant!("workerd", "instant");
        trace_counter!("workerd", "counter", value);
        trace_counter!("workerd", "counter", 1.5);
        assert!(!trace_event_category_enabled!("workerd"));
    }
}
