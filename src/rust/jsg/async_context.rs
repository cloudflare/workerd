// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Async context frames: capture the frame that is current now, and later run code in it, as
//! C++ JSG's `AsyncContextFrame::currentRef()` and `AsyncContextFrame::Scope` do.
//!
//! The current frame is the isolate's continuation-preserved embedder data: `undefined` for the
//! root frame, otherwise the opaque JavaScript wrapper of a C++ `AsyncContextFrame` (see
//! `src/workerd/jsg/async-context.h`). C++ treats any value that is not such a wrapper as the
//! root frame, while Rust, which cannot recognize the wrapper without C++ JSG, treats any value
//! but `undefined` as a frame. The two agree because the embedder data never holds anything
//! else: C++ sets only a frame's wrapper or `undefined`, and this module sets only `undefined`
//! or a value it read from the embedder data, which its private handle type enforces (see
//! [`FrameWrapper`]). V8 carries the value into promise reactions, so a reaction runs
//! in the frame that was current when it was registered, whether JavaScript or Rust registered
//! it. Code that resumes JavaScript from outside a JavaScript continuation, such as the
//! completion of async I/O or a callback that runs later, enters the frame it captured by hand.
//!
//! A captured frame holds the wrapper, and the wrapper owns its C++ frame, so the frame and the
//! values stored in it stay alive for as long as an [`AsyncContextFrame`] does, and for as long
//! as the resource that traces a [`TracedAsyncContextFrame`] does.

#![allow(
    unsafe_code,
    reason = "reads and sets the isolate's continuation-preserved embedder data through the FFI"
)]

use crate::Lock;
use crate::Traced;
use crate::v8::GcVisitor;
use crate::v8::Global;
use crate::v8::Local;
use crate::v8::TracedGlobal;
use crate::v8::ffi;

/// Marker for a handle to a value read from the isolate's continuation-preserved embedder data,
/// which is a C++ frame's wrapper (the root frame's `undefined` is never held).
///
/// It is private, and only [`current_value`] creates handles of it, so the only values that
/// [`set_current`] can make current are `undefined` and ones that were current before. Rust
/// therefore never puts a value in the embedder data that C++ would not recognize as a frame.
struct FrameWrapper;

/// An async context frame that Rust holds outside a resource, for example in a future passed
/// to async I/O, to run the code that resumes after it in the frame that started it.
///
/// It holds a strong [`Global`], so it has no [`Traced`] impl and cannot be a resource field;
/// a resource holds a [`TracedAsyncContextFrame`] instead. Like a `Global`, it is `Send` and
/// `Sync` and may be dropped on any thread. Unless it is the root frame, it panics when entered
/// or cloned in another isolate.
pub struct AsyncContextFrame {
    /// The frame's wrapper, or `None` for the root frame.
    value: Option<Global<FrameWrapper>>,
}

impl AsyncContextFrame {
    /// Captures the frame that is current now.
    pub fn current(lock: &mut Lock<'_>) -> Self {
        Self {
            value: current_value(lock).map(|value| value.to_global(lock)),
        }
    }

    /// The root frame, which holds no stored values.
    #[must_use]
    pub const fn root() -> Self {
        Self { value: None }
    }

    /// Returns whether this is the root frame, captured while the isolate's
    /// continuation-preserved embedder data was `undefined`.
    #[must_use]
    pub const fn is_root(&self) -> bool {
        self.value.is_none()
    }

    /// Creates another handle to the same frame.
    #[must_use]
    pub fn clone(&self, lock: &mut Lock<'_>) -> Self {
        Self {
            value: self.value.as_ref().map(|value| value.clone(lock)),
        }
    }

    /// Runs `f` with this frame current, then makes the frame that was current before current
    /// again, also when `f` returns an error, execution is terminating, or `f` panics.
    ///
    /// `enter` opens no handle scope, so the handles `f` creates stay valid after it. A microtask
    /// checkpoint that `f` runs leaves the root frame current after it, as V8 runs each microtask
    /// in its own frame, the same as under C++ JSG's `AsyncContextFrame::Scope`.
    pub fn enter<'s, R>(&self, lock: &mut Lock<'s>, f: impl FnOnce(&mut Lock<'s>) -> R) -> R {
        let frame = self.value.as_ref().map(|value| value.as_local(lock));
        enter_value(lock, frame, f)
    }
}

/// An async context frame held by a resource field, which the garbage collector traces through
/// the resource, so that a cycle through the frame's stored values can be collected.
///
/// It holds a [`TracedGlobal`]: like any traced state, it must have exactly one owner that the
/// garbage collector traces, so never put it behind an `std::rc::Rc`. It is `Send`, not `Sync`.
/// A frame that Rust holds outside a resource is an [`AsyncContextFrame`]:
/// [`TracedAsyncContextFrame::from`] turns one into one of these, and
/// [`TracedAsyncContextFrame::to_async_context_frame`] makes one of these into one, for example
/// to pass a resource's frame to async I/O.
///
/// Once the resource has no strong reference from Rust, the garbage collector keeps the frame
/// alive only by tracing it through the resource, like any `TracedGlobal`. A frame taken out of
/// its field, for example with `Cell::take`, is traced by nothing, so enter it, clone it or
/// convert it before anything can run JavaScript or allocate on the V8 heap, and put it back.
pub struct TracedAsyncContextFrame {
    /// The frame's wrapper, or `None` for the root frame.
    value: Option<TracedGlobal<FrameWrapper>>,
}

impl TracedAsyncContextFrame {
    /// Captures the frame that is current now.
    pub fn current(lock: &mut Lock<'_>) -> Self {
        AsyncContextFrame::current(lock).into()
    }

    /// The root frame, which holds no stored values.
    #[must_use]
    pub const fn root() -> Self {
        Self { value: None }
    }

    /// Returns whether this is the root frame; see [`AsyncContextFrame::is_root`].
    #[must_use]
    pub const fn is_root(&self) -> bool {
        self.value.is_none()
    }

    /// Creates another handle to the same frame. It is a separate owner, for another resource,
    /// and holds the frame strongly until the garbage collector first traces it.
    #[must_use]
    pub fn clone(&self, lock: &mut Lock<'_>) -> Self {
        Self {
            value: self.value.as_ref().map(|value| value.clone(lock)),
        }
    }

    /// Creates an [`AsyncContextFrame`] for the same frame, which holds it strongly wherever it
    /// goes.
    #[must_use]
    pub fn to_async_context_frame(&self, lock: &mut Lock<'_>) -> AsyncContextFrame {
        AsyncContextFrame {
            value: self
                .value
                .as_ref()
                .map(|value| value.as_local(lock).to_global(lock)),
        }
    }

    /// Runs `f` with this frame current; see [`AsyncContextFrame::enter`].
    pub fn enter<'s, R>(&self, lock: &mut Lock<'s>, f: impl FnOnce(&mut Lock<'s>) -> R) -> R {
        let frame = self.value.as_ref().map(|value| value.as_local(lock));
        enter_value(lock, frame, f)
    }
}

impl From<AsyncContextFrame> for TracedAsyncContextFrame {
    fn from(frame: AsyncContextFrame) -> Self {
        Self {
            value: frame.value.map(TracedGlobal::from),
        }
    }
}

impl Traced for TracedAsyncContextFrame {
    fn trace(&self, visitor: &mut GcVisitor) {
        if let Some(value) = &self.value {
            visitor.visit_traced_global(value);
        }
    }
}

/// The current frame's wrapper, or `None` in the root frame.
fn current_value<'s>(lock: &mut Lock<'s>) -> Option<Local<'s, FrameWrapper>> {
    // SAFETY: the lock keeps the isolate locked with a scope open, which the handle V8 returns
    // belongs to. `FrameWrapper` is only a marker, and the value is the embedder data itself.
    let value = unsafe {
        Local::from_ffi(ffi::isolate_get_continuation_preserved_embedder_data(
            lock.isolate().as_ffi(),
        ))
    };
    (!value.is_undefined()).then_some(value)
}

/// Makes `frame` (`None` for the root frame) current.
fn set_current(lock: &mut Lock<'_>, frame: Option<Local<'_, FrameWrapper>>) {
    let value = match frame {
        // SAFETY: the handle is valid in the lock's scope, and passed straight back to V8.
        Some(frame) => unsafe { frame.into_ffi() },
        // SAFETY: the handle is valid in the lock's scope, and passed straight back to V8.
        None => unsafe { lock.undefined().into_ffi() },
    };
    // SAFETY: the lock keeps the isolate locked, and `value` is a valid handle of its isolate.
    unsafe {
        ffi::isolate_set_continuation_preserved_embedder_data(lock.isolate().as_ffi(), value);
    }
}

/// Runs `f` with `frame` current, and makes the previous frame current again on every exit.
fn enter_value<'s, R>(
    lock: &mut Lock<'s>,
    frame: Option<Local<'s, FrameWrapper>>,
    f: impl FnOnce(&mut Lock<'s>) -> R,
) -> R {
    /// Makes `previous` current again when dropped, also while a panic unwinds.
    struct Restore<'a, 's> {
        lock: &'a mut Lock<'s>,
        previous: Option<Local<'s, FrameWrapper>>,
    }

    impl Drop for Restore<'_, '_> {
        fn drop(&mut self) {
            // Setting the embedder data runs no JavaScript and throws nothing, so this is safe
            // while execution is terminating.
            set_current(self.lock, self.previous);
        }
    }

    let previous = current_value(lock);
    set_current(lock, frame);
    let restore = Restore { lock, previous };
    f(&mut *restore.lock)
}
