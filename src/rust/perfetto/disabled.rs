//! Backend used when Perfetto is not compiled in. Every function has the same signature as in
//! `enabled.rs`. Closures are still type-checked (behind `if false`), so code that builds in one
//! configuration builds in the other, but they're never called and no event is emitted.

use std::ffi::CStr;
use std::marker::PhantomData;

use crate::CounterValue;
use crate::DebugArg;
use crate::Flow;
use crate::Track;

pub const fn register_track_events() {}

pub const fn process_track_uuid() -> u64 {
    0
}

#[inline]
pub const fn is_enabled(_category: usize) -> bool {
    false
}

pub struct EventContext<'a> {
    _marker: PhantomData<&'a mut ()>,
}

impl EventContext<'_> {
    #[expect(clippy::unused_self, reason = "mirrors enabled.rs")]
    pub const fn add_arg(&self, _name: &str, _value: DebugArg<'_>) {}

    #[expect(clippy::unused_self, reason = "mirrors enabled.rs")]
    pub const fn set_track(&self, _track: Track) {}

    #[expect(clippy::unused_self, reason = "mirrors enabled.rs")]
    pub const fn set_flow(&self, _flow: Flow) {}

    #[expect(clippy::unused_self, reason = "mirrors enabled.rs")]
    pub const fn set_terminating_flow(&self, _flow: Flow) {}
}

// Guards code that must type-check but never run.
const TYPE_CHECK_ONLY: bool = false;

#[inline]
fn type_check_only(f: impl FnOnce(EventContext<'_>)) {
    if TYPE_CHECK_ONLY {
        f(EventContext {
            _marker: PhantomData,
        });
    }
}

#[inline]
pub fn emit_instant(_category: usize, _name: &'static CStr, f: impl FnOnce(EventContext<'_>)) {
    type_check_only(f);
}

#[inline]
pub fn emit_begin(
    _category: usize,
    _name: &'static CStr,
    f: impl FnOnce(EventContext<'_>),
) -> bool {
    type_check_only(f);
    false
}

#[inline]
pub fn emit_end(_category: usize, f: impl FnOnce(EventContext<'_>)) {
    type_check_only(f);
}

#[inline]
pub fn emit_counter(_category: usize, _name: &'static str, value: impl FnOnce() -> CounterValue) {
    if TYPE_CHECK_ONLY {
        let _ = value();
    }
}
