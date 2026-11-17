// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! External memory: memory that JavaScript objects keep alive outside the V8 heap, reported to
//! V8 so that it counts toward the isolate's memory use and GC runs when the memory is large.
//!
//! These are C++ JSG's `jsg::ExternalMemoryAdjustment` and `jsg::ExternalMemoryTarget`, shared
//! with it, so that Rust and C++ report to one total. An [`ExternalMemoryAdjustment`] attributes
//! an amount of memory to its isolate until it is dropped. It can be made, changed or dropped on
//! any thread. A thread that has entered the isolate, as every thread holding its lock in workerd
//! has, reports a change to V8 at once; any other thread, or one that has entered another isolate,
//! adds it to a pending total that the isolate hands to V8 the next time an `IsolateBase::Lock`
//! locks it. A decrease reported at once nets a pending rise into it, leaving any remaining rise
//! pending. Pending decreases stay deferred. This path never reports an increase to V8, which
//! could start a GC during destruction or a GC finalizer. After the isolate is destroyed, nothing
//! reaches V8. An [`ExternalMemoryTarget`] makes adjustments without the lock.

use std::ffi::CStr;
use std::fmt;

use kj_rs::KjArc;
use kj_rs::KjOwn;

use crate::Isolate;
use crate::MemoryInfo;
use crate::MemoryTracker;
use crate::v8::ffi;

/// An amount of memory attributed to an isolate, released when dropped: C++
/// `jsg::ExternalMemoryAdjustment`.
///
/// Make one under the lock with [`Isolate::external_memory_adjustment`], or on any thread with
/// [`ExternalMemoryTarget::adjustment`]. A resource that owns memory outside the V8 heap keeps one
/// as a field and sets it to the size of that memory, as a C++ resource does:
///
/// ```ignore
/// #[jsg_resource]
/// struct Buffer {
///     data: RefCell<Vec<u8>>,
///     external_memory: RefCell<jsg::ExternalMemoryAdjustment>,
/// }
///
/// let external_memory = lock.external_memory_adjustment(data.len());
/// ```
///
/// Keep one adjustment per resource for its significant native allocations, updated at
/// construction, growth, and release. Each adjustment allocates an FFI wrapper and holds an atomic
/// reference to the target. Avoid creating one per string or small allocation, or updating it on
/// every small change, since each update crosses the FFI boundary and can call into V8.
///
/// Prefer [`set`](Self::set) when the total allocation size is known; use [`adjust`](Self::adjust)
/// to report a delta. Both can be called on any thread: they report to V8 when the thread has
/// entered the owning isolate, as a thread holding its lock in workerd has, and otherwise defer
/// the change. A decrease can offset a pending increase as described in the module documentation.
///
/// An adjustment of 32 GiB or more, or a change of that much, aborts the process, as in C++ JSG.
pub struct ExternalMemoryAdjustment {
    inner: KjOwn<ffi::ExternalMemoryAdjustment>,
}

// SAFETY: a C++ `ExternalMemoryAdjustment` may be changed and destroyed on any thread: it reports
// to V8 only from a thread that has entered the isolate (`v8::Isolate::TryGetCurrent()`), and
// otherwise adds to the target's atomic pending total. Its amount is only written through
// `&mut self`.
#[expect(unsafe_code, reason = "C++ ExternalMemoryAdjustment is thread-safe")]
unsafe impl Send for ExternalMemoryAdjustment {}
// SAFETY: `&self` only reads the amount.
#[expect(unsafe_code, reason = "C++ ExternalMemoryAdjustment is thread-safe")]
unsafe impl Sync for ExternalMemoryAdjustment {}

impl ExternalMemoryAdjustment {
    /// The amount of memory attributed, in bytes.
    #[must_use]
    pub fn amount(&self) -> usize {
        ffi::external_memory_adjustment_amount(&self.inner)
    }

    /// Changes the amount attributed by `delta` bytes, as C++ `adjust()` does.
    ///
    /// # Errors
    ///
    /// If `delta` would make the amount negative, returns an internal error and leaves the amount
    /// unchanged, matching C++. This indicates a bug in the resource's memory accounting.
    pub fn adjust(&mut self, delta: isize) -> crate::Result<()> {
        ffi::external_memory_adjustment_adjust(self.inner.as_mut(), delta)?;
        Ok(())
    }

    /// Sets the amount attributed to `amount` bytes, as C++ `set()` does. An amount of 32 GiB or
    /// more aborts the process.
    pub fn set(&mut self, amount: usize) {
        ffi::external_memory_adjustment_set(self.inner.as_mut(), amount);
    }
}

impl fmt::Debug for ExternalMemoryAdjustment {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ExternalMemoryAdjustment")
            .field("amount", &self.amount())
            .finish()
    }
}

/// The isolate that adjustments attribute memory to, for making them without the lock: C++
/// `jsg::ExternalMemoryTarget`.
///
/// Get one under the lock with [`Isolate::external_memory_target`]. Clones refer to the same
/// target. It outlives its isolate, after which nothing its adjustments do reaches V8.
#[derive(Clone)]
pub struct ExternalMemoryTarget {
    inner: KjArc<ffi::ExternalMemoryTarget>,
}

// SAFETY: a C++ `ExternalMemoryTarget` is atomically refcounted, and its members that Rust calls
// are const and use atomics; C++ JSG hands it to other threads in the same way.
#[expect(unsafe_code, reason = "C++ ExternalMemoryTarget is thread-safe")]
unsafe impl Send for ExternalMemoryTarget {}
// SAFETY: as for `Send`.
#[expect(unsafe_code, reason = "C++ ExternalMemoryTarget is thread-safe")]
unsafe impl Sync for ExternalMemoryTarget {}

impl ExternalMemoryTarget {
    /// Attributes `amount` bytes to the isolate, as C++ `getAdjustment()` does. Callable on any
    /// thread: without the isolate lock, the amount reaches V8 the next time the isolate is
    /// locked.
    #[must_use]
    pub fn adjustment(&self, amount: usize) -> ExternalMemoryAdjustment {
        ExternalMemoryAdjustment {
            inner: ffi::external_memory_target_adjustment(&self.inner, amount),
        }
    }

    /// Returns false once the isolate is destroyed. True says only that it was alive when asked:
    /// another thread may be destroying it.
    #[must_use]
    pub fn is_isolate_alive(&self) -> bool {
        ffi::external_memory_target_is_isolate_alive(&self.inner)
    }

    /// The total of the changes made without the lock that have not yet reached V8. For tests.
    #[doc(hidden)]
    #[must_use]
    pub fn pending_update_for_test(&self) -> i64 {
        ffi::external_memory_target_pending_update_for_test(&self.inner)
    }
}

impl fmt::Debug for ExternalMemoryTarget {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ExternalMemoryTarget")
            .field("is_isolate_alive", &self.is_isolate_alive())
            .finish()
    }
}

impl Isolate {
    /// Attributes `amount` bytes to this isolate until the returned adjustment is dropped, as C++
    /// `jsg::Lock::getExternalMemoryAdjustment()` does. The amount reaches V8 at once, since a
    /// thread holding the lock in workerd has entered the isolate (debug builds assert it).
    #[expect(unsafe_code, reason = "calls into JSG with the locked isolate")]
    #[must_use]
    pub fn external_memory_adjustment(&mut self, amount: usize) -> ExternalMemoryAdjustment {
        ExternalMemoryAdjustment {
            // SAFETY: the isolate is valid and locked (guaranteed by `&mut self`).
            inner: unsafe { ffi::isolate_external_memory_adjustment(self.as_ffi(), amount) },
        }
    }

    /// Returns this isolate's target, for making adjustments without the lock, as C++
    /// `jsg::Lock::getExternalMemoryTarget()` does.
    #[expect(unsafe_code, reason = "calls into JSG with the locked isolate")]
    #[must_use]
    pub fn external_memory_target(&mut self) -> ExternalMemoryTarget {
        ExternalMemoryTarget {
            // SAFETY: the isolate is valid and locked (guaranteed by `&mut self`).
            inner: unsafe { ffi::isolate_external_memory_target(self.as_ffi()) },
        }
    }
}

// Neither holds a GC reference, so a resource can keep either in a field.
crate::jsg_traced!(ExternalMemoryAdjustment, ExternalMemoryTarget);

// C++ JSG reports neither in heap snapshots: the memory an adjustment counts belongs to its
// owner, which reports it if it reports it at all. Each is reported as plain data: what it owns
// beyond its own bytes is a C++ object of a few words, which C++ JSG doesn't report either.
impl MemoryInfo for ExternalMemoryAdjustment {
    const PLAIN_DATA: bool = true;

    fn track_as_field(&self, _: &mut MemoryTracker<'_>, _: Option<&'static CStr>) {}
}

impl MemoryInfo for ExternalMemoryTarget {
    const PLAIN_DATA: bool = true;

    fn track_as_field(&self, _: &mut MemoryTracker<'_>, _: Option<&'static CStr>) {}
}
