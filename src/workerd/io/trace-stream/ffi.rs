// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The functions `TailStreamTarget` calls.
//!
//! Each takes `params`, a copy of the `TailStreamParams` that `report()` received as a
//! single-segment message (one segment, word-aligned, whose first word is the root pointer), and
//! reads the events from it in place. An error becomes a `kj::Exception` whose
//! description is the error's message.

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, and tail_event_to_js() opens a scope C++ \
              has set up and returns a handle in it"
)]

use capnp::message::ReaderOptions;
use capnp::message::SegmentArray;
use jsg::v8;
use worker_interface_capnp::tail_stream_target::tail_stream_params;

use crate::ConvertError;
use crate::DecodeError;
use crate::Keys;
use crate::Kind;
use crate::Options;
use crate::TailEvent;
use crate::TailEvents;

#[cxx::bridge(namespace = "workerd::tracing::trace_stream")]
mod bridge {
    /// The type of a tail event, as far as `TailStreamTarget` needs to know it.
    enum TailEventKind {
        Onset,
        Outcome,
        Other,
    }

    /// Why `tail_event_to_js()` returned no value without an exception pending, in the cases
    /// where C++ fails as the C++ conversion does.
    enum ConvertFailure {
        /// The conversion succeeded, or an exception is pending.
        None,
        /// The event has an outcome or log level that the schema does not know.
        Unreachable,
        /// The event is a diagnostics channel event whose data has no header, or one of an
        /// unsupported version.
        DiagnosticChannelHeader,
    }

    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");

        type Isolate = jsg::v8::ffi::Isolate;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    #[namespace = "workerd"]
    unsafe extern "C++" {
        include!("workerd/util/thread-scopes.h");

        /// Whether the process runs in predictable mode, for tests.
        ///
        /// cxx names a function's shim by its namespace and Rust name, so the Rust name differs
        /// from that of other bridges that declare the same function.
        #[cxx_name = "isPredictableModeForTest"]
        fn trace_stream_is_predictable_mode_for_test() -> bool;
    }

    extern "Rust" {
        /// The property names a batch has made so far, for `tail_event_to_js()` to reuse.
        type Keys;

        /// An empty `Keys`, for one batch. Holds names of the isolate of the first
        /// `tail_event_to_js()` that uses it, so every call that passes it must be for that
        /// isolate.
        fn new_keys() -> Box<Keys>;

        /// Checks the events of `params`, as decoding each into a `tracing::TailEvent` does,
        /// and returns how many there are.
        fn check_tail_events(params: &[u8]) -> Result<usize>;

        /// The type of the event at `index` of `params`, which `check_tail_events()` accepted.
        fn tail_event_kind(params: &[u8], index: usize) -> Result<TailEventKind>;

        /// The name of the method of a handler object that receives the event at `index` of
        /// `params`, which `check_tail_events()` accepted. Fails for the onset event.
        fn tail_event_handler_name(params: &[u8], index: usize) -> Result<&'static str>;

        /// The JavaScript value of the event at `index` of `params`, which
        /// `check_tail_events()` accepted. Empty if JavaScript rejected a value, in which case
        /// the exception is pending, or if `failure` says why not.
        ///
        /// `isolate` is locked by the calling thread with a context entered and a
        /// `v8::HandleScope` open, which stays open and innermost until the caller consumes the
        /// result.
        unsafe fn tail_event_to_js(
            isolate: *mut Isolate,
            params: &[u8],
            index: usize,
            keys: &mut Keys,
            failure: &mut ConvertFailure,
        ) -> Result<MaybeLocal>;
    }
}

type Result<T> = std::result::Result<T, DecodeError>;

/// Reads `params` and passes its events to `f`, checked if `check`. Calls for one event skip the
/// check of the whole batch, which `check_tail_events()` made, and which would make delivering a
/// batch quadratic in its size.
fn with_events<R>(
    params: &[u8],
    check: bool,
    f: impl FnOnce(TailEvents<'_>) -> Result<R>,
) -> Result<R> {
    let segments = [params];
    // The message was read once already, within the limits of the RPC system, and the copy holds
    // each of its objects once, so it is no larger.
    let options = *ReaderOptions::new().traversal_limit_in_words(None);
    let message = capnp::message::Reader::new(SegmentArray::new(&segments), options);
    let params = message.get_root::<tail_stream_params::Reader<'_>>()?;
    f(if check {
        TailEvents::new(params)?
    } else {
        TailEvents::unchecked(params)?
    })
}

fn event<'a>(events: &TailEvents<'a>, index: usize) -> Result<TailEvent<'a>> {
    events.get(index).ok_or_else(|| {
        DecodeError::new(format!(
            "tail event index {index} is out of range for {} events",
            events.len()
        ))
    })
}

fn check_tail_events(params: &[u8]) -> Result<usize> {
    with_events(params, true, |events| Ok(events.len()))
}

fn tail_event_kind(params: &[u8], index: usize) -> Result<bridge::TailEventKind> {
    with_events(params, false, |events| {
        Ok(match event(&events, index)?.kind() {
            Kind::Onset => bridge::TailEventKind::Onset,
            Kind::Outcome => bridge::TailEventKind::Outcome,
            Kind::Other => bridge::TailEventKind::Other,
        })
    })
}

fn tail_event_handler_name(params: &[u8], index: usize) -> Result<&'static str> {
    with_events(params, false, |events| {
        event(&events, index)?.handler_name().ok_or_else(|| {
            DecodeError::new(
                "Onset event should only be provided to tailStream(), not returned handler",
            )
        })
    })
}

fn new_keys() -> Box<Keys> {
    Box::default()
}

/// # Safety
///
/// As described in the bridge.
unsafe fn tail_event_to_js(
    isolate: *mut bridge::Isolate,
    params: &[u8],
    index: usize,
    keys: &mut Keys,
    failure: &mut bridge::ConvertFailure,
) -> Result<v8::ffi::MaybeLocal> {
    let options = Options {
        predictable: bridge::trace_stream_is_predictable_mode_for_test(),
    };
    // SAFETY: forwarded from this function's safety contract.
    let scope = unsafe { jsg::CallbackScope::new(isolate) };
    let scope = std::pin::pin!(scope);
    jsg::catch_panic(scope, |lock| {
        with_events(params, false, |events| {
            match event(&events, index)?.to_js(lock, keys, options) {
                // SAFETY: the handle lives in the caller's `HandleScope`, which stays open
                // until the caller consumes it, per this function's safety contract.
                Ok(value) => Ok(unsafe { v8::Local::into_ffi_maybe(Some(value)) }),
                Err(ConvertError::Js(err)) => {
                    lock.throw_exception(&err);
                    Ok(v8::ffi::MaybeLocal::default())
                }
                Err(ConvertError::Unreachable) => {
                    *failure = bridge::ConvertFailure::Unreachable;
                    Ok(v8::ffi::MaybeLocal::default())
                }
                Err(ConvertError::DiagnosticChannelHeader) => {
                    *failure = bridge::ConvertFailure::DiagnosticChannelHeader;
                    Ok(v8::ffi::MaybeLocal::default())
                }
                Err(ConvertError::Invalid(err)) => Err(err),
            }
        })
    })
    // A panic left an internal error pending and terminated execution.
    .unwrap_or_else(|| Ok(v8::ffi::MaybeLocal::default()))
}
