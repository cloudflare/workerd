// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "resources are shared with the C++ `Wrappable` through raw pointers"
)]

use std::any::TypeId;
use std::cell::Cell;
use std::collections::HashMap;
use std::ffi::CStr;
use std::fmt;
use std::hash::BuildHasherDefault;
use std::hash::Hasher;
use std::mem::ManuallyDrop;
use std::ptr::NonNull;
use std::sync::Arc;

use kj_rs::KjMaybe;

use crate::ConstantValue;
use crate::Error;
use crate::FeatureFlags;
use crate::FromJS;
use crate::GarbageCollected;
use crate::Intrinsic;
use crate::Lock;
use crate::Member;
use crate::MemoryInfo;
use crate::MemoryTracker;
use crate::ToJS;
use crate::Traced;
use crate::Type;
use crate::UnionMember;
use crate::UnionTypes;
use crate::fast_api::FastCallback;
use crate::feature_flags::compatibility_flags;
use crate::isolate_state::IsolateState;
use crate::isolate_state::Releasable;
use crate::isolate_state::ReleaseContext;
use crate::rtti;
use crate::v8;
use crate::v8::ffi::Wrappable;

/// A reference-counted smart pointer to a Rust resource that integrates with
/// V8's garbage collector.
///
/// Named `Rc` to mirror both [`std::rc::Rc`] (reference-counted shared ownership)
/// and C++ `jsg::Ref` (the GC-integrated reference type in workerd's C++ JSG layer).
/// Like `std::rc::Rc`, cloning is cheap (increments a refcount). Unlike `std::rc::Rc`,
/// the destructor also notifies the GC so JavaScript wrappers can be collected.
///
/// **The isolate lock:** the resource's state and both reference counts are guarded only by the
/// lock of the isolate the resource was created in. So [`get`](Self::get),
/// [`clone`](Self::clone) and [`downgrade`](Self::downgrade) take a [`Lock`] of that isolate,
/// and panic for another isolate's, or once the isolate's `Realm` is dropped. Dropping needs
/// none (see below), and neither do equality, ordering, hashing and `Debug`, which use only the
/// address or the type.
/// An `Rc` may be held where no lock is, such as in a future that `IoContext` polls, which can
/// then do nothing with it but drop it.
///
/// Each `Rc<R>` holds:
/// - An `std::rc::Rc<R>` for Rust-side shared ownership (enables `Weak`)
/// - A [`v8::WrappableRc`] for `kj::Rc` refcounting and GC integration
///
/// **GC integration:**
/// - On [`clone`](Self::clone), calls `wrappable_add_strong_ref` (C++ `addStrongRef`).
/// - On `Drop`, hands its `std::rc::Rc<R>` and `WrappableRc`, with its current `strong` flag,
///   to the [`IsolateState`] of the isolate the resource was created in. Under that isolate's
///   lock they are released at once: `wrappable_remove_strong_ref` (C++ `removeStrongRef`) if
///   the ref is strong, then both handles drop. Without the lock they are queued untouched until
///   the next lock, like C++ `jsg::Ref`'s deferred destruction, since none of their reference
///   counts is atomic.
///
/// **Moving out of a traced field:** once GC has visited an `Rc` in a traced field, the `Rc`
/// gives up its strong reference and relies on the field's owner to trace it. An `Rc` moved
/// out of the field by a bitwise move is then neither strong nor traced, unlike a moved C++
/// `jsg::Ref`, whose move constructor makes it strong again. So a field that an `Rc` is moved
/// out of is a [`crate::Cell`] or [`crate::RefCell`], which make it strong before it leaves
/// the cell, as [`v8::GcVisitor::is_detaching`] describes; `std::cell::Cell` and
/// `std::cell::RefCell` are not `Traced` for types that hold an `Rc`. See `README.md` §Safety
/// rules, rule 2, and <https://github.com/cloudflare/workerd/issues/7976>.
pub struct Rc<R: Resource> {
    /// Shared ownership of the Rust resource. Taken by `Drop`.
    handle: ManuallyDrop<std::rc::Rc<R>>,
    /// Owned, reference-counted handle to the C++ Wrappable. Taken by `Drop`.
    wrappable: ManuallyDrop<v8::WrappableRc>,
    /// The state of the isolate the resource was created in, which releases this reference
    /// under that isolate's lock.
    state: Arc<IsolateState>,
    /// Per-Rc GC state: pointer to the parent `Wrappable`.
    /// `None` until first visited. Set by `visitRef` during GC tracing.
    parent: Cell<Option<NonNull<Wrappable>>>,
    /// Per-Rc GC state: whether this ref is currently strong.
    /// Starts `true` (addStrongRef was called). Toggled by `visitRef` during GC tracing.
    strong: Cell<bool>,
}

impl<R: Resource> Rc<R> {
    /// Creates a new `Rc<R>` that ties a Rust resource's lifetime to the
    /// JavaScript virtual machine's garbage collector.
    ///
    /// This is the primary way to create a Rust resource that can be exposed
    /// to JavaScript. The returned `Rc` can then be converted to a JS object
    /// via [`ToJS::to_js`](crate::ToJS::to_js).
    ///
    /// The resource stays alive as long as at least one `Rc` exists **or** a
    /// JavaScript wrapper object is reachable from the JS heap. When all Rust
    /// `Rc`s are dropped and the JS wrapper becomes unreachable, V8's garbage
    /// collector will destroy the resource.
    ///
    /// The resource belongs to `lock`'s isolate, whose lock its `Rc`s and [`Weak`]s are
    /// released under, wherever they are dropped. Mirrors C++ `js.alloc<T>()`.
    pub fn new(lock: &mut Lock<'_>, resource: R) -> Self {
        let handle = std::rc::Rc::new(resource);

        // Leak an Rc clone as a fat pointer to dyn GarbageCollected and hand
        // ownership to a new Wrappable on the KJ heap. Since R: GarbageCollected,
        // the vtable dispatches trace/memory_name directly to R's implementations.
        let raw: *const R = std::rc::Rc::into_raw(std::rc::Rc::clone(&handle));
        let fat: *mut dyn GarbageCollected = raw as *const dyn GarbageCollected as *mut _;
        let trait_object = v8::ffi::TraitObjectPtr::from_raw(fat, std::any::TypeId::of::<R>());
        let wrappable: v8::WrappableRc = trait_object.into();

        Self::from_parts(handle, wrappable, Arc::clone(lock.isolate_state()))
    }

    /// Makes an `Rc` of a resource and its Wrappable, for which a strong reference has been
    /// added.
    fn from_parts(
        handle: std::rc::Rc<R>,
        wrappable: v8::WrappableRc,
        state: Arc<IsolateState>,
    ) -> Self {
        Self {
            handle: ManuallyDrop::new(handle),
            wrappable: ManuallyDrop::new(wrappable),
            state,
            parent: Cell::new(None),
            strong: Cell::new(true),
        }
    }

    /// Visits this Rc during GC tracing.
    ///
    /// Delegates to C++ `Wrappable::visitRef()` which handles strong/traced switching
    /// and transitive tracing.
    ///
    /// Takes `&self` because `Traced::trace(&self)` receives a shared
    /// reference to `R` inside the `Rc` allocation. The `parent` and `strong`
    /// fields already use `Cell` for interior mutability.
    /// `WrappableRc::visit_rc(&self)` produces `Pin<&mut Wrappable>` from
    /// the raw pointer inside `KjRc` — the mutation target is the C++ heap
    /// object, not the `WrappableRc` wrapper itself.
    pub(crate) fn visit(&self, visitor: &mut v8::GcVisitor) {
        self.wrappable.visit_rc(
            self.parent.as_ptr().cast::<usize>(),
            self.strong.as_ptr(),
            visitor,
        );
    }

    /// The state of the resource's isolate.
    pub(crate) fn isolate_state(&self) -> &IsolateState {
        &self.state
    }

    /// Makes this `Rc` strong again and forgets its parent, as moving a C++ `jsg::Ref` does; see
    /// [`v8::GcVisitor::is_detaching`].
    pub(crate) fn detach(&self) {
        if !self.strong.get() {
            self.wrappable.add_strong_ref();
            self.strong.set(true);
        }
        self.parent.set(None);
    }

    /// Borrows the resource, for no longer than `lock`'s scope.
    ///
    /// # Panics
    /// Panics if `lock` belongs to an isolate other than the resource's.
    #[inline]
    pub fn get<'a, 's: 'a>(&'a self, lock: &Lock<'s>) -> &'a R {
        self.assert_in_isolate_of(lock);
        &self.handle
    }

    /// Makes another strong reference to the resource.
    ///
    /// # Panics
    /// Panics if `lock` belongs to an isolate other than the resource's.
    #[must_use]
    pub fn clone(&self, lock: &Lock<'_>) -> Self {
        self.assert_in_isolate_of(lock);
        let wrappable = v8::WrappableRc::clone(&self.wrappable);
        wrappable.add_strong_ref();
        Self::from_parts(
            std::rc::Rc::clone(&self.handle),
            wrappable,
            Arc::clone(&self.state),
        )
    }

    /// Creates a [`Weak`] reference to this resource.
    ///
    /// The weak reference does not prevent GC collection. Use
    /// [`Weak::upgrade`] to obtain a strong `Rc<R>` if the resource is
    /// still alive.
    ///
    /// Mirrors [`std::rc::Rc::downgrade`].
    ///
    /// # Panics
    /// Panics if `lock` belongs to an isolate other than the resource's.
    #[must_use]
    pub fn downgrade(&self, lock: &Lock<'_>) -> Weak<R> {
        self.assert_in_isolate_of(lock);
        Weak {
            handle: std::rc::Rc::downgrade(&self.handle),
            wrappable: Some(self.wrappable.as_ptr()),
            state: Some(Arc::clone(&self.state)),
        }
    }

    #[inline]
    fn assert_in_isolate_of(&self, lock: &Lock<'_>) {
        assert!(
            is_isolate_of(&self.state, lock),
            "attempt to use a jsg::Rc in an isolate other than the one it was created in"
        );
    }

    /// Attaches this resource to the `this` object in a V8 constructor callback.
    ///
    /// Called from `#[jsg_constructor]`-generated code. V8 has already created
    /// the `this` object from the `FunctionTemplate`'s `InstanceTemplate`;
    /// this method attaches the `Wrappable` to it so that instance methods
    /// can resolve the resource via `resolve_resource`.
    ///
    /// # Panics
    /// Panics if `lock` belongs to an isolate other than the resource's, if `info` is not a
    /// construct call of a resource type's constructor, or if its `this` already wraps a
    /// resource: an object keeps the resource it wraps, which its methods borrow while they run.
    pub fn attach_to_this(&self, lock: &Lock<'_>, info: &mut v8::FunctionCallbackInfo<'_>) {
        self.assert_in_isolate_of(lock);
        self.wrappable.attach_to_this(info);
    }

    /// Returns the C++ Wrappable's strong reference count.
    #[cfg(debug_assertions)]
    pub fn strong_refcount(&self, lock: &Lock<'_>) -> u32 {
        self.assert_in_isolate_of(lock);
        self.wrappable.strong_refcount()
    }
}

impl<R: Resource> Drop for Rc<R> {
    fn drop(&mut self) {
        // SAFETY: `drop` runs once, and neither field is used after it is taken.
        let (handle, wrappable) = unsafe {
            (
                ManuallyDrop::take(&mut self.handle),
                ManuallyDrop::take(&mut self.wrappable),
            )
        };
        self.state.release(ReleasedRc {
            strong: self.strong.get(),
            handle,
            wrappable,
        });
    }
}

/// The parts of a dropped [`Rc`], which its isolate's state releases (see [`IsolateState`]).
struct ReleasedRc<R: Resource> {
    /// Whether the `Rc` was counted in the Wrappable's strong refcount.
    strong: bool,
    #[expect(dead_code, reason = "held to be dropped")]
    handle: std::rc::Rc<R>,
    wrappable: v8::WrappableRc,
}

impl<R: Resource> Releasable for ReleasedRc<R> {
    fn release(mut self, context: ReleaseContext) {
        // A ref that GC tracing has made weak no longer counts as strong; removing it again
        // would release another ref's strong reference. Once the isolate is disposed there is
        // no wrapper left to update.
        if self.strong && context == ReleaseContext::Locked {
            self.wrappable.remove_strong_ref();
        }
        // Then `handle` and `wrappable` drop, which may destroy the Wrappable and the resource.
    }
}

/// The `std::rc::Weak` of a dropped [`Weak`] only has its weak count to give back.
impl<R: Resource> Releasable for std::rc::Weak<R> {
    fn release(self, _context: ReleaseContext) {}
}

impl<'s, R: Resource + 'static> ToJS<'s> for Rc<R> {
    fn to_js(self, lock: &mut Lock<'s>) -> Result<v8::Local<'s, v8::Value>, Error> {
        wrap(lock, self)
    }
}

impl<R: Resource> Type for Rc<R> {
    // A Web IDL non-callback interface type.
    const UNION_TYPES: UnionTypes = UnionTypes::INTERFACE_LIKE;
    // The resource's, which `#[jsg_resource]` makes its module path and name.
    const UNION_NAMES: &'static [&'static str] = R::UNION_NAMES;

    fn class_name() -> &'static str {
        R::class_name()
    }

    // Any object: telling whether it wraps an `R` takes a lock, which `is_exact` doesn't have.
    // `FromJS::try_from_js` makes that check. A resource is not `Coercible`, so no
    // `NonCoercible<Rc<R>>` relies on this.
    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_object()
    }
}

impl<'s, R: Resource + 'static> FromJS<'s> for Rc<R> {
    type ResultType = Self;

    const UNION_MEMBER: UnionMember = UnionMember::Resource;
    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = false;

    fn type_name() -> String {
        R::class_name().to_owned()
    }

    fn from_js(lock: &mut Lock<'s>, value: v8::Local<'s, v8::Value>) -> Result<Self, Error> {
        let type_name = value.type_of(lock);
        Self::try_from_js(lock, value)?.ok_or_else(|| {
            Error::new_type_error(format!("expected {}, got {type_name}", R::class_name()))
        })
    }

    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> Result<Option<Self>, Error> {
        let Some(wrappable) = v8::WrappableRc::from_js(lock, value) else {
            return Ok(None);
        };
        let Some(resource_ptr) = wrappable.resolve_resource::<R>() else {
            return Ok(None);
        };

        // SAFETY: The pointer came from std::rc::Rc::into_raw in Rc::new(), and the
        // Wrappable being alive guarantees the Rc allocation is still valid.
        // increment_strong_count bumps the count so from_raw can safely create
        // a second Rc handle without double-freeing.
        let handle = unsafe {
            std::rc::Rc::increment_strong_count(resource_ptr.as_ptr());
            std::rc::Rc::from_raw(resource_ptr.as_ptr())
        };

        wrappable.add_strong_ref();
        Ok(Some(Self::from_parts(
            handle,
            wrappable,
            Arc::clone(lock.isolate_state()),
        )))
    }
}

/// Equality by pointer identity — two `Rc`s are equal iff they point to the
/// same allocation, matching the behaviour of [`std::rc::Rc`].
impl<R: Resource> PartialEq for Rc<R> {
    fn eq(&self, other: &Self) -> bool {
        std::rc::Rc::ptr_eq(&self.handle, &other.handle)
    }
}

impl<R: Resource> Eq for Rc<R> {}

/// Ordering by pointer address — consistent with `PartialEq` and allows
/// `Rc<T>` to be used as a `BTreeSet`/`BTreeMap` key.
impl<R: Resource> PartialOrd for Rc<R> {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl<R: Resource> Ord for Rc<R> {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        std::rc::Rc::as_ptr(&self.handle).cmp(&std::rc::Rc::as_ptr(&other.handle))
    }
}

/// Hashing by pointer address — consistent with `PartialEq` and allows
/// `Rc<T>` to be used as a `HashSet`/`HashMap` key.
impl<R: Resource> std::hash::Hash for Rc<R> {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        std::rc::Rc::as_ptr(&self.handle).hash(state);
    }
}

/// Shows only the resource's type: its reference counts can't be read without the lock.
impl<R: Resource> fmt::Debug for Rc<R> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Rc")
            .field("type", &std::any::type_name::<R>())
            .finish_non_exhaustive()
    }
}

/// A weak reference to a resource that doesn't prevent resource destruction.
///
/// Uses `Weak<R>` for liveness detection: when all `Rc`s drop and
/// `~Wrappable` drops the `Rc` (via `std::rc::Rc::from_raw`), the `std::rc::Rc<R>` reaches
/// 0 and all `Weak`s expire.
///
/// Does NOT hold a `WrappableRc` — does not keep the `kj::Rc<Wrappable>` alive.
/// Stores a non-owning pointer to the Wrappable so that `upgrade()` can
/// reconstruct a `WrappableRc` via `wrappable_to_rc`. The pointer is only
/// dereferenced after `Weak::upgrade` confirms the resource (and thus the
/// Wrappable) is still alive.
///
/// The counts of the `std::rc::Weak<R>` are not atomic either, so, as for an [`Rc`], every
/// operation but `Drop` takes a [`Lock`] of the resource's isolate, and a drop hands the
/// `std::rc::Weak` to the state of that isolate, which releases it under the isolate's lock. The
/// Wrappable may be gone by then, so the `Weak` keeps the state itself.
pub struct Weak<R: Resource> {
    handle: std::rc::Weak<R>,
    /// Non-owning pointer to the C++ Wrappable. `None` for default-constructed
    /// `Weak`s (which can never be upgraded). Valid as long as the resource
    /// is alive (verified by `Weak::upgrade` before use).
    wrappable: Option<NonNull<Wrappable>>,
    /// The state of the isolate the resource was created in. `None` for default-constructed
    /// `Weak`s, whose `std::rc::Weak` owns no allocation.
    state: Option<Arc<IsolateState>>,
}

impl<R: Resource> Weak<R> {
    /// Returns `true` if the resource is still alive.
    ///
    /// # Panics
    /// Panics if `lock` belongs to an isolate other than the resource's.
    #[inline]
    pub fn is_alive(&self, lock: &Lock<'_>) -> bool {
        self.assert_in_isolate_of(lock);
        self.handle.strong_count() > 0
    }

    /// Upgrades to a strong `Rc<R>` if the resource is still alive.
    ///
    /// # Panics
    /// Panics if `lock` belongs to an isolate other than the resource's.
    pub fn upgrade(&self, lock: &Lock<'_>) -> Option<Rc<R>> {
        self.assert_in_isolate_of(lock);
        let handle = self.handle.upgrade()?;
        let wrappable_ptr = self.wrappable?;
        let state = Arc::clone(self.state.as_ref()?);
        // Reconstruct a WrappableRc from the stored wrappable pointer.
        // SAFETY: The resource is alive (Weak::upgrade succeeded), so the Wrappable
        // is alive too (the Wrappable's Rc clone keeps the resource alive, which
        // means the Wrappable must still exist — its destruction via std::rc::Rc::from_raw
        // would have released the Rc and thus the resource).
        let wrappable = unsafe { v8::WrappableRc::from_raw_wrappable(wrappable_ptr.as_ptr()) };
        wrappable.add_strong_ref();
        Some(Rc::from_parts(handle, wrappable, state))
    }

    /// Makes another weak reference to the resource.
    ///
    /// # Panics
    /// Panics if `lock` belongs to an isolate other than the resource's.
    #[must_use]
    pub fn clone(&self, lock: &Lock<'_>) -> Self {
        self.assert_in_isolate_of(lock);
        Self {
            handle: std::rc::Weak::clone(&self.handle),
            wrappable: self.wrappable,
            state: self.state.clone(),
        }
    }

    /// A default-constructed `Weak` belongs to no isolate, and has no count to guard.
    #[inline]
    fn assert_in_isolate_of(&self, lock: &Lock<'_>) {
        if let Some(state) = &self.state {
            assert!(
                is_isolate_of(state, lock),
                "attempt to use a jsg::Weak in an isolate other than the one it was created in"
            );
        }
    }
}

/// Whether `lock` locks the isolate of `state`. A closed state's isolate has no `Realm` left, so
/// no lock of it exists, though a new isolate may have its address.
#[inline]
fn is_isolate_of(state: &IsolateState, lock: &Lock<'_>) -> bool {
    !state.is_closed() && state.belongs_to(lock.isolate().as_ffi())
}

impl<R: Resource> Drop for Weak<R> {
    fn drop(&mut self) {
        if let Some(state) = &self.state {
            // `std::rc::Weak::new()`, left in its place, owns no allocation.
            state.release(std::mem::take(&mut self.handle));
        }
    }
}

// SAFETY: the resource, its Wrappable and the counts of both are shared by every `Rc` and `Weak`
// of the resource, and guarded only by the isolate lock. What touches them takes a `&Lock` of the
// isolate, and panics for another's (`get`, `clone`, `downgrade`, `to_js`, `attach_to_this`,
// `strong_refcount`); is a GC or detaching visit, which `GcVisitor::visit_rc` lets change the
// counts only under the lock of the resource's isolate; or is `Drop`, which releases the parts
// under that lock and otherwise queues them without touching a count. `Lock` is neither `Send`
// nor `Sync`. Equality, ordering and hashing use only the address, and `Debug` only the
// type. `R: Send`, so the resource may be dropped by whichever thread releases it.
unsafe impl<R: Resource + Send> Send for Rc<R> {}

// SAFETY: as for `Rc`: `is_alive`, `upgrade` and `clone` take a `&Lock`, and `Drop` releases or
// queues the `std::rc::Weak` as an `Rc`'s drop does. The Wrappable pointer is used only by
// `upgrade`.
unsafe impl<R: Resource + Send> Send for Weak<R> {}

/// `Rc<R>` is a strong GC edge — visited via `GcVisitor::visit_rc`.
impl<R: Resource> Traced for Rc<R> {
    fn trace(&self, visitor: &mut v8::GcVisitor) {
        visitor.visit_rc(self);
    }
}

// Weak references don't keep the target alive and have no GC edges to trace.
crate::jsg_traced!(<R: Resource> Weak<R>);

/// The resource's node, as C++ reports a `jsg::Ref`; nothing for a resource of another isolate,
/// whose wrapper the snapshot cannot read.
impl<R: Resource> MemoryInfo for Rc<R> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        if !tracker.can_track_object_of(&self.state) {
            return;
        }
        self.wrappable.track(tracker, edge_name);
    }
}

/// A weak reference retains nothing.
impl<R: Resource> MemoryInfo for Weak<R> {
    fn track_as_field(&self, _: &mut MemoryTracker<'_>, _: Option<&'static CStr>) {}
}

impl<R: Resource> Default for Weak<R> {
    fn default() -> Self {
        Self {
            handle: std::rc::Weak::new(),
            wrappable: None,
            state: None,
        }
    }
}

/// Wraps a `Rc<R>` as a JavaScript object backed by the resource's `FunctionTemplate`.
///
/// **Identity**: Multiple calls with `Rc`s that share the same underlying `Wrappable`
/// (i.e. clones of the same `Rc`) return the **same** JS object — V8 caches the
/// wrapper on the `Wrappable` via `CppgcShim`. This means:
///
/// ```text
/// let a = resource.clone(lock).to_js(lock)?;
/// let b = resource.to_js(lock)?;
/// // a === b in JavaScript (same object identity)
/// ```
///
/// **Prototype**: The returned object's prototype is set up from the resource's
/// [`FunctionTemplate`], which includes all `#[jsg_method]` instance methods and
/// `#[jsg_static_constant]` values registered via [`Resource::members`].
///
/// **Ownership**: The `Rc` is consumed. The JS wrapper keeps the `Wrappable`
/// alive via `CppgcShim`. When the JS object is garbage collected and all Rust
/// `Rc`s are dropped, the resource is destroyed.
///
/// # Errors
///
/// If V8 fails to create the wrapper, or an exception or a termination is already pending, the
/// exception stays pending and the error is one for which [`Error::is_pending`] is true.
///
/// # Panics
/// Panics if the resource was created in another isolate (see [`Rc::new`]), or already has a
/// JavaScript wrapper in one. The `Rc` is leaked in that case.
#[expect(
    clippy::panic,
    reason = "using a wrapper in another isolate would mix the isolates' heaps"
)]
pub fn wrap<'s, R: Resource + 'static>(
    lock: &mut Lock<'s>,
    resource: Rc<R>,
) -> Result<v8::Local<'s, v8::Value>, Error> {
    if Arc::ptr_eq(&resource.state, lock.isolate_state())
        && let Some(wrapper) = resource.wrappable.to_js::<R>(lock)?
    {
        return Ok(wrapper);
    }
    // Dropping `resource` here could release its strong reference on a wrapper of another
    // isolate, which needs that isolate's lock. It is leaked in a box instead, which keeps
    // pointers to the resource and its Wrappable for LeakSanitizer to follow.
    let leaked = Box::leak(Box::new(resource));
    // SAFETY: `leaked` is a live heap allocation.
    unsafe { v8::ffi::ignore_leak(std::ptr::from_mut(leaked).expose_provenance()) };
    panic!("attempt to wrap a resource in an isolate other than the one it was created in");
}

/// Returns the constructor function for a resource type as a `Local<Function>`.
///
/// Lazily creates and caches the `FunctionTemplate` in the `Realm` on first call.
/// The returned function can be exposed as a global (e.g., `ctx.set_global("MyClass", func)`).
///
/// # Errors
///
/// If an exception or a termination is pending, fails with an error for which
/// [`Error::is_pending`] is true, as [`v8::Global::<v8::FunctionTemplate>::as_local_function`]
/// does.
pub fn function_template_of<'s, R: Resource + 'static>(
    lock: &mut Lock<'s>,
) -> Result<v8::Local<'s, v8::Function>, Error> {
    let constructor = lock.realm().get_constructor::<R>().as_ffi();
    // SAFETY: the constructor is a function template of the lock's isolate, which the Realm's
    // template cache keeps alive.
    unsafe { v8::function_template_get_function(lock, &constructor) }
}

/// Returns whether `value` is the JavaScript wrapper of a resource of type `R`.
///
/// Unlike converting `value` with [`FromJS`], this creates no [`Rc<R>`](Rc), so it is the check
/// to use when only the type matters: for example, in a function that C++ calls to check a value
/// it holds (`README.md` §Creating resources from C++).
pub fn is<R: Resource + 'static>(lock: &mut Lock<'_>, value: v8::Local<'_, v8::Value>) -> bool {
    v8::WrappableRc::from_js(lock, value)
        .is_some_and(|wrappable| wrappable.resolve_resource::<R>().is_some())
}

/// [`is`] for a function that C++ calls, as [`v8::run_ffi_callback`] is for one that returns a
/// handle: opens the caller's scope and runs [`is`] through [`catch_panic`](crate::catch_panic).
///
/// Like `v8::Maybe<bool>`, the result is none when an exception is pending: here, when the check
/// panicked, which leaves an internal error pending and execution terminating. C++ propagates it
/// as `jsg::JsExceptionThrown` rather than treating it as a mismatch, since throwing another
/// exception would replace the termination.
///
/// # Safety
///
/// `isolate` must point to a live V8 isolate locked by the current thread with a context entered
/// and a `HandleScope` open, and `value` must be a handle in that scope.
pub unsafe fn ffi_is<R: Resource + 'static>(
    isolate: *mut v8::ffi::Isolate,
    value: v8::ffi::Local,
) -> KjMaybe<bool> {
    // SAFETY: forwarded from this function's safety contract.
    let scope = std::pin::pin!(unsafe { crate::CallbackScope::new(isolate) });
    crate::catch_panic(scope, |lock| {
        // SAFETY: `value` is a handle in the scope that `lock` stands for, per this function's
        // safety contract.
        let value = unsafe { v8::Local::from_ffi(value) };
        is::<R>(lock, value)
    })
    .into()
}

/// The registry through which template creation collects the members of a resource type.
struct TemplateMembers {
    /// The type's class name, for the panics of template creation.
    class_name: &'static str,
    members: Vec<Member>,
    inherit_intrinsic: Option<Intrinsic>,
    parent: Option<ParentType>,
}

impl TemplateMembers {
    /// Panics unless the type inherits from nothing yet: `FunctionTemplate::Inherit` takes one
    /// parent, whether a resource type or an intrinsic.
    fn assert_no_parent(&self) {
        assert!(
            self.inherit_intrinsic.is_none() && self.parent.is_none(),
            "{} inherits from more than one type or intrinsic",
            self.class_name
        );
    }
}

impl MemberRegistry for TemplateMembers {
    fn add(&mut self, member: impl MemberDefinition) {
        self.members.push(member.into_member());
    }

    fn ts_override(&mut self, _ts_override: &'static str) {}

    fn ts_define(&mut self, _ts_define: &'static str) {}

    fn inherit_intrinsic(&mut self, intrinsic: Intrinsic) {
        self.assert_no_parent();
        self.inherit_intrinsic = Some(intrinsic);
    }

    fn inherit(&mut self, parent: ParentType) {
        self.assert_no_parent();
        self.parent = Some(parent);
    }
}

/// Builds a [`ResourceDescriptor`] from the members that `R`'s
/// [`Resource::members`] registers under `flags`.
/// The descriptor is passed to C++ `create_resource_template` to set up the
/// V8 `FunctionTemplate` with the correct methods, constants, and constructor.
/// Nested types and the parent type are returned separately, since their templates are
/// installed once the template for `R` exists.
#[expect(
    clippy::too_many_lines,
    reason = "one match arm per Member variant, each a field-by-field conversion"
)]
fn get_resource_descriptor<R: Resource>(flags: compatibility_flags::Reader<'_>) -> Registration {
    let mut nested_types = Vec::new();
    let mut descriptor = v8::ffi::ResourceDescriptor {
        name: R::class_name().to_owned(),
        constructor: KjMaybe::None,
        methods: Vec::new(),
        properties: Vec::new(),
        static_methods: Vec::new(),
        static_constants: Vec::new(),
        wildcard_property: KjMaybe::None,
        dispose: KjMaybe::None,
        async_dispose: KjMaybe::None,
        callable: KjMaybe::None,
        iterable: KjMaybe::None,
        inherit_intrinsic: KjMaybe::None,
    };

    let mut members = TemplateMembers {
        class_name: R::class_name(),
        members: Vec::new(),
        inherit_intrinsic: None,
        parent: None,
    };
    R::members(&mut members, flags);
    descriptor.inherit_intrinsic = members.inherit_intrinsic.into();
    let parent = members.parent;
    let mut wildcard = None;
    for m in members.members {
        match m {
            Member::Constructor { callback, length } => {
                descriptor.constructor = KjMaybe::Some(v8::ffi::ConstructorDescriptor {
                    callback: callback as usize,
                    length,
                });
            }
            Member::Method {
                name,
                callback,
                fast,
                length,
            } => {
                descriptor.methods.push(v8::ffi::MethodDescriptor {
                    name,
                    callback: callback as usize,
                    fast: fast.map(FastCallback::into_ffi).into(),
                    length,
                });
            }
            Member::Property {
                name,
                kind,
                getter_callback,
                setter_callback,
            } => {
                // Instance properties take PropertyCallbackInfo-style callbacks.
                assert!(
                    kind != v8::ffi::PropertyKind::Instance,
                    "use Member::InstanceProperty"
                );
                assert!(
                    kind != v8::ffi::PropertyKind::LazyInstance
                        && kind != v8::ffi::PropertyKind::LazyReadonlyInstance,
                    "use Member::LazyInstanceProperty; a lazy instance property has no setter"
                );
                descriptor.properties.push(v8::ffi::PropertyDescriptor {
                    name,
                    kind,
                    getter_callback: getter_callback as usize,
                    setter_callback: setter_callback.map(|f| f as usize).into(),
                });
            }
            Member::InstanceProperty {
                name,
                getter_callback,
                setter_callback,
            } => {
                descriptor.properties.push(v8::ffi::PropertyDescriptor {
                    name,
                    kind: v8::ffi::PropertyKind::Instance,
                    getter_callback: getter_callback as usize,
                    setter_callback: setter_callback.map(|f| f as usize).into(),
                });
            }
            Member::LazyInstanceProperty {
                name,
                getter_callback,
                read_only,
            } => descriptor
                .properties
                .push(lazy_instance_property_descriptor(
                    name,
                    getter_callback,
                    read_only,
                )),
            Member::StaticMethod {
                name,
                callback,
                fast,
                length,
            } => {
                descriptor.static_methods.push(v8::ffi::MethodDescriptor {
                    name,
                    callback: callback as usize,
                    fast: fast.map(FastCallback::into_ffi).into(),
                    length,
                });
            }
            Member::StaticConstant { name, value } => {
                let ConstantValue::Number(number_value) = value;
                descriptor
                    .static_constants
                    .push(v8::ffi::StaticConstantDescriptor {
                        name,
                        value: number_value,
                    });
            }
            Member::WildcardProperty {
                getter_callback,
                query_callback,
            } => {
                assert!(
                    wildcard.is_none(),
                    "{} has more than one wildcard property",
                    R::class_name()
                );
                wildcard = Some(v8::ffi::WildcardPropertyDescriptor {
                    getter_callback: getter_callback as usize,
                    query_callback: query_callback as usize,
                });
            }
            Member::Dispose { callback } => {
                assert!(
                    descriptor.dispose.is_none(),
                    "{} has more than one dispose method",
                    R::class_name()
                );
                descriptor.dispose = KjMaybe::Some(callback as usize);
            }
            Member::AsyncDispose { callback } => {
                assert!(
                    descriptor.async_dispose.is_none(),
                    "{} has more than one async dispose method",
                    R::class_name()
                );
                descriptor.async_dispose = KjMaybe::Some(callback as usize);
            }
            Member::Callable { callback } => {
                assert!(
                    descriptor.callable.is_none(),
                    "{} is callable more than one way",
                    R::class_name()
                );
                descriptor.callable = KjMaybe::Some(callback as usize);
            }
            Member::Iterable { callback } => {
                assert!(
                    descriptor.iterable.is_none(),
                    "{} has more than one iterator method",
                    R::class_name()
                );
                descriptor.iterable = KjMaybe::Some(callback as usize);
            }
            Member::NestedType { name, nested_type } => nested_types.push((name, nested_type)),
        }
    }

    Registration {
        descriptor,
        wildcard,
        nested_types,
        parent,
    }
}

/// What [`get_resource_descriptor`] collects from a resource type's registration.
struct Registration {
    /// The descriptor, without its wildcard property, which may come from the parent type.
    descriptor: v8::ffi::ResourceDescriptor,
    /// The type's own wildcard property.
    wildcard: Option<v8::ffi::WildcardPropertyDescriptor>,
    nested_types: NestedTypes,
    parent: Option<ParentType>,
}

/// The descriptor of a [`Member::LazyInstanceProperty`].
fn lazy_instance_property_descriptor(
    name: String,
    getter_callback: crate::RawGetterCallback,
    read_only: bool,
) -> v8::ffi::PropertyDescriptor {
    v8::ffi::PropertyDescriptor {
        name,
        kind: if read_only {
            v8::ffi::PropertyKind::LazyReadonlyInstance
        } else {
            v8::ffi::PropertyKind::LazyInstance
        },
        getter_callback: getter_callback as usize,
        setter_callback: KjMaybe::None,
    }
}

/// The nested types of a resource type, with their names.
type NestedTypes = Vec<(String, NestedType)>;

/// How a resource type whose template is being created reaches the next one.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Edge {
    /// The template is the one asked for, not reached through another type.
    Root,
    /// The previous type nests this one ([`Member::NestedType`]).
    Nests,
    /// The previous type inherits from this one ([`MemberRegistry::inherit`]).
    Inherits,
}

/// The resource types whose templates are being created, each nesting or inheriting from the
/// next, with their Rust type names and how each was reached.
type Nesting = Vec<(TypeId, &'static str, Edge)>;

/// Gets or creates the template of a resource type and returns its index in
/// [`Resources::templates`].
type TemplateSlot =
    fn(&mut Resources, v8::IsolatePtr, &Arc<IsolateState>, &FeatureFlags, &mut Nesting) -> usize;

/// A resource type that another resource type exposes as a nested type
/// ([`Member::NestedType`]).
///
/// A resource type must not nest itself, directly or through other nested
/// types; creating its template panics.
pub struct NestedType {
    template: TemplateSlot,
    describe: fn(&mut rtti::Context) -> &'static str,
}

impl NestedType {
    /// The nested type `R`.
    #[must_use]
    pub fn of<R: Resource + rtti::TypeInfo>() -> Self {
        Self {
            template: |resources, isolate, state, flags, nesting| {
                resources.template_slot::<R>(isolate, state, flags, nesting, Edge::Nests)
            },
            describe: rtti::Context::describe_structure::<R>,
        }
    }
}

/// A resource type that another resource type inherits from, like the `Base` of C++ JSG's
/// `JSG_INHERIT(Base)`; see [`MemberRegistry::inherit`].
///
/// The child's template `Inherit`s the parent's, so the parent's prototype is the [[Prototype]]
/// of the child's, and the parent's methods, which have the parent's `v8::Signature`, accept the
/// child's instances. A child without a wildcard property of its own gets the parent's, which
/// V8 does not inherit. A callable parent's call handler is not inherited.
///
/// Neither makes the parent's callbacks accept a child's instance as an `R`: they resolve their
/// receiver by its exact type.
///
/// A resource type must not inherit from itself, directly or through other types, or nest a type
/// that inherits from it, which V8 would instantiate recursively; creating its template panics.
pub struct ParentType {
    template: TemplateSlot,
    describe: fn(&mut rtti::Context) -> rtti::Type,
}

impl ParentType {
    /// The parent type `R`.
    #[must_use]
    pub fn of<R: Resource + rtti::TypeInfo>() -> Self {
        Self {
            template: |resources, isolate, state, flags, nesting| {
                resources.template_slot::<R>(isolate, state, flags, nesting, Edge::Inherits)
            },
            describe: R::describe,
        }
    }

    /// The parent's structure, as RTTI describes it.
    pub(crate) fn describe(&self, cx: &mut rtti::Context) -> rtti::Type {
        (self.describe)(cx)
    }
}

/// Hashes a [`TypeId`], whose `Hash` implementation writes a single `u64` that is already a
/// high-quality hash, so this passes it through rather than hashing it again.
#[derive(Default)]
struct TypeIdHasher(u64);

impl Hasher for TypeIdHasher {
    fn write_u64(&mut self, value: u64) {
        self.0 = value;
    }

    // `TypeId` only calls `write_u64`; this keeps any other input correct.
    fn write(&mut self, bytes: &[u8]) {
        for &byte in bytes {
            self.0 = (self.0.rotate_left(8) ^ u64::from(byte)).wrapping_mul(0x0100_0000_01b3);
        }
    }

    fn finish(&self) -> u64 {
        self.0
    }
}

#[derive(Default)]
pub struct Resources {
    /// The index in `templates` of each resource type's template.
    slots: HashMap<TypeId, usize, BuildHasherDefault<TypeIdHasher>>,
    /// Cached V8 `FunctionTemplate`s, which GC never visits.
    templates: Vec<v8::Global<v8::FunctionTemplate>>,
    /// The wildcard property of the template at the same index, own or inherited, which a child
    /// template without one of its own gets.
    wildcards: Vec<Option<v8::ffi::WildcardPropertyDescriptor>>,
}

impl Resources {
    /// Gets or creates the cached `FunctionTemplate` for a resource type.
    ///
    /// `flags` are only read when the template is first created.
    pub(crate) fn get_constructor<R: Resource + 'static>(
        &mut self,
        isolate: v8::IsolatePtr,
        state: &Arc<IsolateState>,
        flags: &FeatureFlags,
    ) -> &v8::Global<v8::FunctionTemplate> {
        let slot = self.template_slot::<R>(isolate, state, flags, &mut Vec::new(), Edge::Root);
        &self.templates[slot]
    }

    /// Gets or creates the cached `FunctionTemplate` for a resource type, reached through `edge`
    /// while the templates of the types in `nesting` are being created, and returns its index
    /// in `templates`.
    ///
    /// # Panics
    ///
    /// Panics if `R`'s template is being created already, meaning that `R`
    /// directly or indirectly nests or inherits from itself.
    #[expect(
        clippy::panic,
        reason = "a resource type nesting or inheriting from itself is a programming error"
    )]
    fn template_slot<R: Resource + 'static>(
        &mut self,
        isolate: v8::IsolatePtr,
        state: &Arc<IsolateState>,
        flags: &FeatureFlags,
        nesting: &mut Nesting,
        edge: Edge,
    ) -> usize {
        let type_id = TypeId::of::<R>();
        if let Some(&slot) = self.slots.get(&type_id) {
            return slot;
        }
        let name = std::any::type_name::<R>();
        if let Some(start) = nesting.iter().position(|&(id, _, _)| id == type_id) {
            let path: Vec<_> = nesting.iter().map(|&(_, name, _)| name).collect();
            let edges = nesting[start + 1..]
                .iter()
                .map(|&(_, _, edge)| edge)
                .chain([edge]);
            let relation = if edges.clone().all(|edge| edge == Edge::Nests) {
                "nests itself"
            } else if edges.clone().all(|edge| edge == Edge::Inherits) {
                "inherits from itself"
            } else {
                "nests a type that inherits from it"
            };
            panic!("resource type {relation}: {} -> {name}", path.join(" -> "));
        }
        nesting.push((type_id, name, edge));
        let (template, wildcard) =
            self.create_resource_constructor::<R>(isolate, state, flags, nesting);
        nesting.pop();
        let slot = self.templates.len();
        self.templates.push(template);
        self.wildcards.push(wildcard);
        self.slots.insert(type_id, slot);
        slot
    }

    /// Creates a new V8 `FunctionTemplate` for resource type `R` via the C++ FFI,
    /// creating the templates of its parent and nested types as needed. Returns it with its
    /// wildcard property, own or inherited.
    fn create_resource_constructor<R: Resource>(
        &mut self,
        isolate: v8::IsolatePtr,
        state: &Arc<IsolateState>,
        flags: &FeatureFlags,
        nesting: &mut Nesting,
    ) -> (
        v8::Global<v8::FunctionTemplate>,
        Option<v8::ffi::WildcardPropertyDescriptor>,
    ) {
        let Registration {
            mut descriptor,
            mut wildcard,
            nested_types,
            parent,
        } = get_resource_descriptor::<R>(flags.reader());
        // The parent's template is created first, since `Inherit` must be called before the
        // child's template is instantiated.
        let parent_slot =
            parent.map(|parent| (parent.template)(self, isolate, state, flags, nesting));
        if let Some(slot) = parent_slot {
            wildcard = wildcard.or(self.wildcards[slot]);
        }
        descriptor.wildcard_property = wildcard.into();
        // SAFETY: Caller guarantees the isolate is valid and locked.
        let handle = unsafe { v8::ffi::create_resource_template(isolate.as_ffi(), &descriptor) };
        let template = v8::Global::from_ffi_in(handle, Arc::clone(state));
        if let Some(slot) = parent_slot {
            // SAFETY: The caller guarantees the isolate is valid and locked, and both templates
            // are live `Global<FunctionTemplate>`s of this isolate. The template for `R` was just
            // created, so it has not been instantiated.
            unsafe {
                v8::ffi::function_template_inherit(
                    isolate.as_ffi(),
                    &template.as_ffi(),
                    &self.templates[slot].as_ffi(),
                );
            }
        }
        for (name, nested_type) in nested_types {
            let slot = (nested_type.template)(self, isolate, state, flags, nesting);
            // SAFETY: The caller guarantees the isolate is valid and locked, and both
            // templates are live `Global<FunctionTemplate>`s of this isolate. The template
            // for `R` has not been instantiated yet, so it can still be modified.
            unsafe {
                v8::ffi::function_template_set_nested_type(
                    isolate.as_ffi(),
                    &template.as_ffi(),
                    &name,
                    &self.templates[slot].as_ffi(),
                );
            }
        }
        (template, wildcard)
    }

    /// Drains every cached resource-template handle, transferring ownership of each
    /// `v8::Global<v8::FunctionTemplate>` to the caller. Used by the `PREPARE_SNAPSHOT`
    /// pipeline, which disposes each handle before `CreateBlob`; the cache is left empty and
    /// templates are recreated lazily on demand.
    pub fn take_template_handles(&mut self) -> Vec<v8::ffi::Global> {
        self.slots.clear();
        self.wildcards.clear();
        self.templates
            .drain(..)
            .map(v8::Global::into_ffi_for_snapshot)
            .collect()
    }
}

/// Receives the members that [`Resource::members`] registers.
///
/// Template creation collects each member's callbacks, while RTTI export collects its
/// JavaScript type description, so both follow the same registration code, like the
/// `Registry` parameter of C++ `registerMembers`.
pub trait MemberRegistry {
    fn add(&mut self, member: impl MemberDefinition);

    /// Customizes the resource's generated TypeScript declaration, like C++
    /// `JSG_TS_OVERRIDE`. Replaces any override registered earlier or given by
    /// `#[jsg_resource(ts_override = "...")]`. Template creation ignores it.
    fn ts_override(&mut self, ts_override: &'static str);

    /// Emits TypeScript declarations next to the resource's declaration, like C++
    /// `JSG_TS_DEFINE`. Replaces any definitions registered earlier or given by
    /// `#[jsg_resource(ts_define = "...")]`. Template creation ignores it.
    fn ts_define(&mut self, ts_define: &'static str);

    /// Makes `intrinsic` the [[Prototype]] of the resource's prototype, like C++
    /// `JSG_INHERIT_INTRINSIC`, so that instances inherit its methods; RTTI writes it as the
    /// structure's `extends`. `#[jsg_resource(inherit_intrinsic = ...)]` calls it with automatic
    /// registration. A type inherits from at most one type or intrinsic: template creation
    /// panics if this or [`inherit`](Self::inherit) is called twice.
    fn inherit_intrinsic(&mut self, intrinsic: Intrinsic);

    /// Makes `parent`'s prototype the [[Prototype]] of the resource's prototype, like C++
    /// `JSG_INHERIT`; RTTI writes it as the structure's `extends`. See [`ParentType`] for what
    /// the resource inherits. `#[jsg_resource(inherit = Parent)]` calls it with automatic
    /// registration. A type inherits from at most one type or intrinsic: template creation
    /// panics if this or [`inherit_intrinsic`](Self::inherit_intrinsic) is called twice.
    fn inherit(&mut self, parent: ParentType);
}

/// A member that [`Resource::members`] can register: the result of a generated member factory,
/// a [`nested_type`], or an [`undescribed`] member built by hand.
pub trait MemberDefinition {
    /// Builds the member for template creation.
    fn into_member(self) -> Member;

    /// Describes the member for RTTI, or returns `None` if the member has no TypeScript
    /// declaration.
    fn describe(self, cx: &mut rtti::Context) -> Option<rtti::Member>;
}

/// A member that RTTI omits from the TypeScript declarations; see [`undescribed`].
pub struct Undescribed(Member);

/// Registers `member`, which is built by hand, without a TypeScript declaration.
pub fn undescribed(member: Member) -> Undescribed {
    Undescribed(member)
}

impl MemberDefinition for Undescribed {
    fn into_member(self) -> Member {
        self.0
    }

    fn describe(self, _cx: &mut rtti::Context) -> Option<rtti::Member> {
        None
    }
}

/// A nested resource type, which describes itself; see [`nested_type`].
pub struct NestedTypeMember {
    name: String,
    nested_type: NestedType,
}

/// Registers the resource type `R` as a nested type, exposed as a constructor property named
/// `name`.
pub fn nested_type<R: Resource + rtti::TypeInfo>(name: impl Into<String>) -> NestedTypeMember {
    NestedTypeMember {
        name: name.into(),
        nested_type: NestedType::of::<R>(),
    }
}

impl MemberDefinition for NestedTypeMember {
    fn into_member(self) -> Member {
        Member::NestedType {
            name: self.name,
            nested_type: self.nested_type,
        }
    }

    fn describe(self, cx: &mut rtti::Context) -> Option<rtti::Member> {
        Some(rtti::Member::Nested {
            name: self.name,
            identity: (self.nested_type.describe)(cx),
        })
    }
}

/// A member definition that builds the member and describes its type on demand. Template
/// creation never runs the description, and RTTI export never builds the member.
pub struct DescribedMember<M, D> {
    member: M,
    describe: D,
}

/// Returns a member definition built by `member` and described by `describe`.
pub fn described_member<M, D>(member: M, describe: D) -> DescribedMember<M, D>
where
    M: FnOnce() -> Member,
    D: FnOnce(&mut rtti::Context) -> rtti::Member,
{
    DescribedMember { member, describe }
}

impl<M, D> MemberDefinition for DescribedMember<M, D>
where
    M: FnOnce() -> Member,
    D: FnOnce(&mut rtti::Context) -> rtti::Member,
{
    fn into_member(self) -> Member {
        (self.member)()
    }

    fn describe(self, cx: &mut rtti::Context) -> Option<rtti::Member> {
        Some((self.describe)(cx))
    }
}

/// Rust types exposed to JavaScript as resource types.
///
/// Resource types are passed by reference and call back into Rust when JavaScript accesses
/// their members. This is analogous to `JSG_RESOURCE_TYPE` in C++ JSG. Resources must provide
/// member declarations, a cleanup function for GC, and access to their V8 wrapper state.
pub trait Resource: Type + GarbageCollected + Sized + 'static {
    /// Registers the methods, properties, and constructors exposed to JavaScript.
    ///
    /// `flags` are the compatibility flags of the isolate the template is built for, or of the
    /// RTTI export, so the members can vary with them, as with the
    /// `CompatibilityFlags::Reader flags` parameter of C++ `JSG_RESOURCE_TYPE`. Templates are
    /// cached per resource type in the realm. Use `#[jsg_resource(custom_members)]` to generate
    /// member factories while supplying this implementation yourself.
    fn members(registry: &mut impl MemberRegistry, flags: compatibility_flags::Reader<'_>);
}

#[cfg(test)]
#[path = "resource-test.rs"]
mod tests;
