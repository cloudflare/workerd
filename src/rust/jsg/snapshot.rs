// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Resources that a V8 startup snapshot can carry: the Rust side of C++
//! `Wrappable::snapshotClone()`, `snapshotReferences()` and `snapshotRelink()`
//! (`src/workerd/jsg/wrappable.h`). See `docs/startup-snapshot.md`.

#![expect(
    unsafe_code,
    reason = "a copy owns a resource through a raw trait object pointer, and a restored \
              reference is rebuilt from the Wrappable C++ passes"
)]

use std::any::TypeId;
use std::ffi::CStr;

use crate::GarbageCollected;
use crate::HoldsNoGcReferences;
use crate::Lock;
use crate::Rc;
use crate::Resource;
use crate::Traced;
use crate::v8::GcVisitor;
use crate::v8::ffi::TraitObjectPtr;
use crate::v8::ffi::Wrappable;

/// A resource whose instances a startup snapshot can carry, as a C++ JSG type does by overriding
/// `Wrappable::snapshotClone()`.
///
/// The zygote that builds the snapshot calls [`snapshot`](Self::snapshot) on each live instance
/// its top level left reachable, and stores the result. Each Worker started from the snapshot
/// calls [`restore`](Self::restore) on it, and attaches the instance that returns to the restored
/// wrapper. A resource type that does not implement this trait makes such a Worker start without
/// the snapshot.
///
/// The stored [`Snapshot`](Self::Snapshot) belongs to no isolate: it is made in the zygote, and
/// read on the threads of the Workers started from it, so it is `Send` and `Sync` and holds no GC
/// reference, nor any `v8::Global`, which is not [`Traced`]. Nor may it hold request state, such
/// as an `IoOwned`.
///
/// A restored instance is built afresh, so a `jsg::Rc` it holds would refer to a new resource
/// rather than to the one behind the restored wrapper JavaScript sees. A type whose references
/// JavaScript can observe (an accessor that returns the same object each time, for example) lists
/// them in [`snapshot_references`](Self::snapshot_references), and takes the restored ones in
/// [`snapshot_relink`](Self::snapshot_relink).
///
/// None of these methods may panic: C++ calls them, so a panic aborts the process. A type whose
/// instance cannot be copied does not implement this trait, which makes the Worker start without
/// the snapshot.
pub trait SnapshotTransfer: Resource {
    /// What the snapshot stores of an instance.
    type Snapshot: HoldsNoGcReferences + Send + Sync + 'static;

    /// The state of this instance to store in the snapshot. Called in the zygote, under the
    /// isolate lock.
    #[must_use]
    fn snapshot(&self) -> Self::Snapshot;

    /// A new instance from what [`snapshot`](Self::snapshot) stored. Called once per Worker
    /// started from the snapshot, while its context is created; it may run on any thread, and
    /// takes no lock, so it creates no `jsg::Rc` or V8 value.
    #[must_use]
    fn restore(snapshot: &Self::Snapshot) -> Self;

    /// Adds the resources this instance holds a `jsg::Rc` to whose identity JavaScript can
    /// observe. Called in the zygote, on the live original. It must not create JavaScript
    /// values or resources.
    fn snapshot_references(&self, _references: &mut SnapshotReferences) {}

    /// Takes, in the order [`snapshot_references`](Self::snapshot_references) added them in the
    /// zygote, the restored resources this instance refers to. Called in a Worker started from
    /// the snapshot on the instance [`restore`](Self::restore) returned, once every restored
    /// wrapper is linked.
    fn snapshot_relink(&self, _lock: &mut Lock<'_>, _restored: &mut RestoredReferences<'_>) {}
}

/// The references a resource lists for a startup snapshot; see
/// [`SnapshotTransfer::snapshot_references`].
pub struct SnapshotReferences {
    wrappables: Vec<usize>,
}

impl SnapshotReferences {
    /// Adds a reference, or `None` for one the instance does not hold, which a Worker started
    /// from the snapshot restores as `None`.
    pub fn add<'a, R: Resource + 'a>(&mut self, reference: impl Into<Option<&'a Rc<R>>>) {
        self.wrappables
            .push(reference.into().map_or(0, |rc| rc.wrappable_ptr() as usize));
    }
}

/// The restored resources an instance refers to; see [`SnapshotTransfer::snapshot_relink`].
pub struct RestoredReferences<'a> {
    wrappables: std::slice::Iter<'a, usize>,
}

impl RestoredReferences<'_> {
    /// The restored resource behind the next reference that
    /// [`snapshot_references`](SnapshotTransfer::snapshot_references) added. `None` if that was
    /// `None`, if the snapshot carried no wrapper for the resource, which the instance then keeps
    /// its own for, or if the resource is not an `R`.
    pub fn next<R: Resource>(&mut self, lock: &mut Lock<'_>) -> Option<Rc<R>> {
        let wrappable = *self.wrappables.next()?;
        if wrappable == 0 {
            return None;
        }
        // SAFETY: C++ passes the Wrappables of restored wrappers, which stay alive while the
        // context is created.
        unsafe { Rc::from_restored_wrappable(lock, wrappable as *const Wrappable) }
    }
}

/// The type-erased [`SnapshotTransfer`] of a resource, which [`GarbageCollected::snapshot_transfer`]
/// returns.
#[doc(hidden)]
pub trait ErasedSnapshotTransfer {
    /// The object to store in the snapshot, owned by no `Wrappable` yet.
    fn snapshot_copy(&self) -> SnapshotCopy;

    /// The Wrappables of what [`SnapshotTransfer::snapshot_references`] adds; 0 for `None`.
    fn snapshot_references(&self) -> Vec<usize> {
        Vec::new()
    }

    /// Calls [`SnapshotTransfer::snapshot_relink`] with the Wrappables C++ passes; 0 for none.
    fn snapshot_relink(&self, _lock: &mut Lock<'_>, _restored: &[usize]) {}
}

impl<R: SnapshotTransfer> ErasedSnapshotTransfer for R {
    fn snapshot_copy(&self) -> SnapshotCopy {
        SnapshotCopy::of(Stored::<R> {
            snapshot: self.snapshot(),
            name: self.memory_name(),
        })
    }

    fn snapshot_references(&self) -> Vec<usize> {
        let mut references = SnapshotReferences {
            wrappables: Vec::new(),
        };
        SnapshotTransfer::snapshot_references(self, &mut references);
        references.wrappables
    }

    fn snapshot_relink(&self, lock: &mut Lock<'_>, restored: &[usize]) {
        SnapshotTransfer::snapshot_relink(
            self,
            lock,
            &mut RestoredReferences {
                wrappables: restored.iter(),
            },
        );
    }
}

/// What the snapshot stores of a resource: the `Wrappable` of the stored copy owns one. Never
/// wrapped, so never traced; it has the resource's name, which C++ checks.
struct Stored<R: SnapshotTransfer> {
    snapshot: R::Snapshot,
    name: &'static CStr,
}

impl<R: SnapshotTransfer> Traced for Stored<R> {
    fn trace(&self, _visitor: &mut GcVisitor) {}
}

impl<R: SnapshotTransfer> GarbageCollected for Stored<R> {
    fn memory_name(&self) -> &'static CStr {
        self.name
    }

    fn snapshot_transfer(&self) -> Option<&dyn ErasedSnapshotTransfer> {
        Some(self)
    }
}

impl<R: SnapshotTransfer> ErasedSnapshotTransfer for Stored<R> {
    fn snapshot_copy(&self) -> SnapshotCopy {
        SnapshotCopy::of(R::restore(&self.snapshot))
    }
}

/// A resource, or what a snapshot stores of one, owned by no `Wrappable` yet.
#[doc(hidden)]
pub struct SnapshotCopy(TraitObjectPtr);

impl SnapshotCopy {
    fn of<T: GarbageCollected + 'static>(value: T) -> Self {
        let raw: *const T = std::rc::Rc::into_raw(std::rc::Rc::new(value));
        let fat: *mut dyn GarbageCollected = raw as *const dyn GarbageCollected as *mut _;
        Self(TraitObjectPtr::from_raw(fat, TypeId::of::<T>()))
    }

    /// The trait object of the copy, for a new `Wrappable` to own.
    pub(crate) fn into_trait_object(self) -> TraitObjectPtr {
        let copy = std::mem::ManuallyDrop::new(self);
        copy.0.clone()
    }
}

impl Drop for SnapshotCopy {
    fn drop(&mut self) {
        // SAFETY: the pointer came from `Rc::into_raw` in `of`, and no `Wrappable` took it.
        unsafe { self.0.drop_rc() };
    }
}
