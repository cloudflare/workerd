use std::cell::Cell;
use std::fmt;
use std::ops::Deref;
use std::ptr::NonNull;
use std::rc::Rc;
use std::rc::Weak;

use crate::GarbageCollected;
use crate::Lock;
use crate::Resource;
use crate::v8;
use crate::v8::ffi::Wrappable;

/// Allocates a new resource inside a `Wrappable` on the KJ heap.
///
/// Creates an `Rc<R>` for shared ownership, then leaks a clone via
/// `Rc::into_raw` to obtain a `*const R`. This thin pointer is coerced
/// to `*mut dyn GarbageCollected` (a fat pointer carrying `R`'s vtable)
/// and stored in the Wrappable's `data[0..1]`. When `~Wrappable` runs,
/// `wrappable_invoke_drop` reconstructs the `Rc` via `Rc::from_raw` and
/// drops it, releasing the refcount.
pub fn alloc<R: Resource + 'static>(lock: &Lock, resource: R) -> Ref<R> {
    let _ = lock;

    let rc = Rc::new(resource);

    // Leak an Rc clone as a fat pointer to dyn GarbageCollected and hand
    // ownership to a new Wrappable on the KJ heap. Since R: GarbageCollected,
    // the vtable dispatches trace/get_name directly to R's implementations.
    let raw: *const R = Rc::into_raw(Rc::clone(&rc));
    let fat: *mut dyn GarbageCollected = raw as *const dyn GarbageCollected as *mut _;
    let wrappable: v8::WrappableRc = v8::TraitObjectPtr::from_raw(fat).into();

    Ref {
        rc,
        wrappable,
        parent: Cell::new(None),
        strong: Cell::new(true),
    }
}

/// A strong reference to a Rust resource managed by the C++ Wrappable system.
///
/// Each `Ref<R>` holds:
/// - An `Rc<R>` for Rust-side shared ownership (enables `WeakRef`)
/// - A [`v8::WrappableRc`] for `kj::Rc` refcounting and GC integration
///
/// **GC integration:**
/// - On `Clone`, calls `wrappable_add_strong_ref` (C++ `addStrongRef`).
/// - On `Drop`, calls `wrappable_remove_strong_ref` (C++ `removeStrongRef` +
///   `maybeDeferDestruction`), then fields drop in declaration order.
pub struct Ref<R: Resource> {
    /// Shared ownership of the Rust resource.
    rc: Rc<R>,
    /// Owned, reference-counted handle to the C++ Wrappable.
    wrappable: v8::WrappableRc,
    /// Per-Ref GC state: pointer to the parent `Wrappable`.
    /// `None` until first visited. Set by `visitRef` during GC tracing.
    parent: Cell<Option<NonNull<Wrappable>>>,
    /// Per-Ref GC state: whether this ref is currently strong.
    /// Starts `true` (addStrongRef was called). Toggled by `visitRef` during GC tracing.
    strong: Cell<bool>,
}

impl<R: Resource> Ref<R> {
    /// Visits this Ref during GC tracing.
    ///
    /// Delegates to C++ `Wrappable::visitRef()` which handles strong/traced switching
    /// and transitive tracing.
    ///
    /// Takes `&self` because `GarbageCollected::trace(&self)` receives a shared
    /// reference to `R` inside the `Rc` allocation. The `parent` and `strong`
    /// fields already use `Cell` for interior mutability.
    /// `WrappableRc::visit_ref(&self)` produces `Pin<&mut Wrappable>` from
    /// the raw pointer inside `KjRc` — the mutation target is the C++ heap
    /// object, not the `WrappableRc` wrapper itself.
    pub(crate) fn visit(&self, visitor: &mut v8::GcVisitor) {
        self.wrappable.visit_ref(
            self.parent.as_ptr().cast::<usize>(),
            self.strong.as_ptr(),
            visitor,
        );
    }
}

impl<R: Resource> Deref for Ref<R> {
    type Target = R;

    #[inline]
    fn deref(&self) -> &Self::Target {
        &self.rc
    }
}

impl<R: Resource> Clone for Ref<R> {
    fn clone(&self) -> Self {
        let mut wrappable = self.wrappable.clone();
        wrappable.add_strong_ref();
        Self {
            rc: Rc::clone(&self.rc),
            wrappable,
            parent: Cell::new(None),
            strong: Cell::new(true),
        }
    }
}

impl<R: Resource> Drop for Ref<R> {
    fn drop(&mut self) {
        self.wrappable.remove_strong_ref();
    }
}

impl<R: Resource> fmt::Debug for Ref<R> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Ref")
            .field("type", &std::any::type_name::<R>())
            .field("rc_strong", &Rc::strong_count(&self.rc))
            .finish_non_exhaustive()
    }
}

/// A weak reference to a resource that doesn't prevent resource destruction.
///
/// Uses `Weak<R>` for liveness detection: when all `Ref`s drop and
/// `~Wrappable` drops the `Rc` (via `Rc::from_raw`), the `Rc<R>` reaches
/// 0 and all `Weak`s expire.
///
/// Does NOT hold a `WrappableRc` — does not keep the `kj::Rc<Wrappable>` alive.
/// Stores a non-owning pointer to the Wrappable so that `upgrade()` can
/// reconstruct a `WrappableRc` via `wrappable_to_rc`. The pointer is only
/// dereferenced after `Weak::upgrade` confirms the resource (and thus the
/// Wrappable) is still alive.
pub struct WeakRef<R: Resource> {
    weak: Weak<R>,
    /// Non-owning pointer to the C++ Wrappable. `None` for default-constructed
    /// `WeakRef`s (which can never be upgraded). Valid as long as the resource
    /// is alive (verified by `Weak::upgrade` before use).
    wrappable: Option<NonNull<Wrappable>>,
}

impl<R: Resource> WeakRef<R> {
    /// Returns `true` if the resource is still alive.
    #[inline]
    pub fn is_alive(&self) -> bool {
        self.weak.strong_count() > 0
    }

    /// Upgrades to a strong `Ref<R>` if the resource is still alive.
    pub fn upgrade(&self) -> Option<Ref<R>> {
        let rc = self.weak.upgrade()?;
        let wrappable_ptr = self.wrappable?;
        // Reconstruct a WrappableRc from the stored wrappable pointer.
        // SAFETY: The resource is alive (Weak::upgrade succeeded), so the Wrappable
        // is alive too (the Wrappable's Rc clone keeps the resource alive, which
        // means the Wrappable must still exist — its destruction via Rc::from_raw
        // would have released the Rc and thus the resource).
        let mut wrappable = unsafe { v8::WrappableRc::from_raw_wrappable(wrappable_ptr.as_ptr()) };
        wrappable.add_strong_ref();
        Some(Ref {
            rc,
            wrappable,
            parent: Cell::new(None),
            strong: Cell::new(true),
        })
    }
}

impl<R: Resource> From<&Ref<R>> for WeakRef<R> {
    fn from(r: &Ref<R>) -> Self {
        Self {
            weak: Rc::downgrade(&r.rc),
            wrappable: Some(r.wrappable.as_ptr()),
        }
    }
}

impl<R: Resource> Clone for WeakRef<R> {
    fn clone(&self) -> Self {
        Self {
            weak: Weak::clone(&self.weak),
            wrappable: self.wrappable,
        }
    }
}

impl<R: Resource> GarbageCollected for WeakRef<R> {
    /// No-op trace for `WeakRef`s. Weak references don't participate in GC tracing
    /// since they don't keep objects alive (the resource is controlled by `Rc<R>`).
    fn trace(&self, _visitor: &mut v8::GcVisitor) {}
}

impl<R: Resource> Default for WeakRef<R> {
    fn default() -> Self {
        Self {
            weak: Weak::new(),
            wrappable: None,
        }
    }
}

#[expect(clippy::needless_pass_by_value)]
pub fn wrap<'a, R: Resource + 'static>(
    lock: &mut Lock,
    resource: Ref<R>,
) -> v8::Local<'a, v8::Value> {
    let isolate = lock.isolate();
    let constructor = lock.realm().get_constructor::<R>(isolate);

    resource.wrappable.to_js(isolate, constructor)
}

/// Unwraps a JavaScript value to get a mutable reference to the Rust resource.
///
/// Returns `None` if the value is not a Rust-wrapped resource (e.g. a plain JS
/// object, a C++ JSG object, or a value that was never wrapped by the Rust layer).
///
/// # Safety
/// The caller must ensure the type `R` matches the actual resource type.
pub unsafe fn unwrap<'a, R: Resource>(
    lock: &'a mut Lock,
    value: v8::Local<v8::Value>,
) -> Option<&'a mut R> {
    let wrappable = v8::WrappableRc::from_js(lock.isolate(), value)?;
    let resource_ptr = wrappable.resolve_resource::<R>();
    Some(unsafe { &mut *resource_ptr.as_ptr() })
}

/// Unwraps a JavaScript value to get a `Ref<R>` to the Rust resource.
///
/// Returns `None` if the value is not a Rust-wrapped resource (e.g. a plain JS
/// object, a C++ JSG object, or a value that was never wrapped by the Rust layer).
///
/// Resolves the resource pointer from the Wrappable's `data[0]` (which is the
/// `Rc::into_raw` pointer), then reconstructs an `Rc<R>` via
/// `Rc::increment_strong_count` + `Rc::from_raw`.
///
/// # Safety
/// The caller must ensure the type `R` matches the actual resource type.
pub unsafe fn unwrap_ref<R: Resource>(
    lock: &mut Lock,
    value: v8::Local<v8::Value>,
) -> Option<Ref<R>> {
    let mut wrappable = v8::WrappableRc::from_js(lock.isolate(), value)?;
    let resource_ptr = wrappable.resolve_resource::<R>();

    // Reconstruct Rc<R> from the raw pointer.
    // resolve_resource returns data[0] which is the Rc::into_raw pointer.
    // SAFETY: The pointer came from Rc::into_raw in alloc(), and the
    // Wrappable being alive guarantees the Rc allocation is still valid.
    // increment_strong_count bumps the count so from_raw can safely create
    // a second Rc handle without double-freeing.
    let rc = unsafe {
        Rc::increment_strong_count(resource_ptr.as_ptr());
        Rc::from_raw(resource_ptr.as_ptr())
    };

    wrappable.add_strong_ref();
    Some(Ref {
        rc,
        wrappable,
        parent: Cell::new(None),
        strong: Cell::new(true),
    })
}
