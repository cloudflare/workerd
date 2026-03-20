// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::any::TypeId;
use std::cell::Cell;
use std::collections::HashMap;
use std::fmt;
use std::ops::Deref;
use std::ptr::NonNull;

use kj_rs::KjMaybe;

use crate::ConstantValue;
use crate::Error;
use crate::FromJS;
use crate::GarbageCollected;
use crate::Lock;
use crate::Member;
use crate::ToJS;
use crate::Type;
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
/// Each `Rc<R>` holds:
/// - An `std::rc::Rc<R>` for Rust-side shared ownership (enables `Weak`)
/// - A [`v8::WrappableRc`] for `kj::Rc` refcounting and GC integration
///
/// **GC integration:**
/// - On `Clone`, calls `wrappable_add_strong_ref` (C++ `addStrongRef`).
/// - On `Drop`, calls `wrappable_remove_strong_ref` with the ref's current
///   `strong` flag (C++ `maybeDeferDestruction`), then fields drop in
///   declaration order.
pub struct Rc<R: Resource> {
    /// Shared ownership of the Rust resource.
    handle: std::rc::Rc<R>,
    /// Owned, reference-counted handle to the C++ Wrappable.
    wrappable: v8::WrappableRc,
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
    pub fn new(resource: R) -> Self {
        let handle = std::rc::Rc::new(resource);

        // Leak an Rc clone as a fat pointer to dyn GarbageCollected and hand
        // ownership to a new Wrappable on the KJ heap. Since R: GarbageCollected,
        // the vtable dispatches trace/get_name directly to R's implementations.
        let raw: *const R = std::rc::Rc::into_raw(std::rc::Rc::clone(&handle));
        let fat: *mut dyn GarbageCollected = raw as *const dyn GarbageCollected as *mut _;
        let trait_object = v8::ffi::TraitObjectPtr::from_raw(fat, std::any::TypeId::of::<R>());
        let wrappable: v8::WrappableRc = trait_object.into();

        Self {
            handle,
            wrappable,
            parent: Cell::new(None),
            strong: Cell::new(true),
        }
    }

    /// Visits this Rc during GC tracing.
    ///
    /// Delegates to C++ `Wrappable::visitRef()` which handles strong/traced switching
    /// and transitive tracing.
    ///
    /// Takes `&self` because `GarbageCollected::trace(&self)` receives a shared
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

    /// Creates a [`Weak`] reference to this resource.
    ///
    /// The weak reference does not prevent GC collection. Use
    /// [`Weak::upgrade`] to obtain a strong `Rc<R>` if the resource is
    /// still alive.
    ///
    /// Mirrors [`std::rc::Rc::downgrade`](std::rc::std::rc::Rc::downgrade).
    pub fn downgrade(&self) -> Weak<R> {
        Weak::from(self)
    }

    /// Attaches this resource to the `this` object in a V8 constructor callback.
    ///
    /// Called from `#[jsg_constructor]`-generated code. V8 has already created
    /// the `this` object from the `FunctionTemplate`'s `InstanceTemplate`;
    /// this method attaches the `Wrappable` to it so that instance methods
    /// can resolve the resource via `resolve_resource`.
    pub fn attach_to_this(&self, info: &mut v8::FunctionCallbackInfo) {
        self.wrappable.attach_to_this(info);
    }

    /// Returns the C++ Wrappable's strong reference count.
    #[cfg(debug_assertions)]
    pub fn strong_refcount(&self) -> u32 {
        self.wrappable.strong_refcount()
    }
}

impl<R: Resource> Deref for Rc<R> {
    type Target = R;

    #[inline]
    fn deref(&self) -> &Self::Target {
        &self.handle
    }
}

impl<R: Resource> Clone for Rc<R> {
    fn clone(&self) -> Self {
        let mut wrappable = self.wrappable.clone();
        wrappable.add_strong_ref();
        Self {
            handle: std::rc::Rc::clone(&self.handle),
            wrappable,
            parent: Cell::new(None),
            strong: Cell::new(true),
        }
    }
}

impl<R: Resource> Drop for Rc<R> {
    fn drop(&mut self) {
        self.wrappable.remove_strong_ref(self.strong.get());
    }
}

impl<R: Resource + 'static> ToJS for Rc<R> {
    fn to_js<'a, 'b>(self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        wrap(lock, self)
    }
}

impl<R: Resource + 'static> FromJS for Rc<R> {
    type ResultType = Self;

    fn from_js(lock: &mut Lock, value: v8::Local<v8::Value>) -> Result<Self, Error> {
        // Capture the JS type name before consuming the value, for error messages.
        let type_name = value.type_of();

        let mut wrappable = v8::WrappableRc::from_js(lock.isolate(), value).ok_or_else(|| {
            Error::new_type_error(format!("expected {}, got {type_name}", R::class_name()))
        })?;

        let resource_ptr = wrappable.resolve_resource::<R>().ok_or_else(|| {
            Error::new_type_error(format!("expected {}, got {type_name}", R::class_name()))
        })?;

        // SAFETY: The pointer came from std::rc::Rc::into_raw in Rc::new(), and the
        // Wrappable being alive guarantees the Rc allocation is still valid.
        // increment_strong_count bumps the count so from_raw can safely create
        // a second Rc handle without double-freeing.
        let handle = unsafe {
            std::rc::Rc::increment_strong_count(resource_ptr.as_ptr());
            std::rc::Rc::from_raw(resource_ptr.as_ptr())
        };

        wrappable.add_strong_ref();
        Ok(Self {
            handle,
            wrappable,
            parent: Cell::new(None),
            strong: Cell::new(true),
        })
    }
}

impl<R: Resource> fmt::Debug for Rc<R> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Rc")
            .field("type", &std::any::type_name::<R>())
            .field("rc_strong", &std::rc::Rc::strong_count(&self.handle))
            .finish_non_exhaustive()
    }
}

/// A weak reference to a resource that doesn't prevent resource destruction.
///
/// Uses `Weak<R>` for liveness detection: when all `Rc`s drop and
/// `~Wrappable` drops the `Rc` (via `std::rc::Rc::from_raw`), the `std::rc::Rc<R>` reaches
/// 0 and all `Weak`s expire.
///
/// Does NOT hold a `WrappableRc` — does not keep the `kj::std::rc::Rc<Wrappable>` alive.
/// Stores a non-owning pointer to the Wrappable so that `upgrade()` can
/// reconstruct a `WrappableRc` via `wrappable_to_rc`. The pointer is only
/// dereferenced after `Weak::upgrade` confirms the resource (and thus the
/// Wrappable) is still alive.
pub struct Weak<R: Resource> {
    weak: std::rc::Weak<R>,
    /// Non-owning pointer to the C++ Wrappable. `None` for default-constructed
    /// `Weak`s (which can never be upgraded). Valid as long as the resource
    /// is alive (verified by `Weak::upgrade` before use).
    wrappable: Option<NonNull<Wrappable>>,
}

impl<R: Resource> Weak<R> {
    /// Returns `true` if the resource is still alive.
    #[inline]
    pub fn is_alive(&self) -> bool {
        self.weak.strong_count() > 0
    }

    /// Upgrades to a strong `Rc<R>` if the resource is still alive.
    pub fn upgrade(&self) -> Option<Rc<R>> {
        let handle = self.weak.upgrade()?;
        let wrappable_ptr = self.wrappable?;
        // Reconstruct a WrappableRc from the stored wrappable pointer.
        // SAFETY: The resource is alive (Weak::upgrade succeeded), so the Wrappable
        // is alive too (the Wrappable's Rc clone keeps the resource alive, which
        // means the Wrappable must still exist — its destruction via std::rc::Rc::from_raw
        // would have released the Rc and thus the resource).
        let mut wrappable = unsafe { v8::WrappableRc::from_raw_wrappable(wrappable_ptr.as_ptr()) };
        wrappable.add_strong_ref();
        Some(Rc {
            handle,
            wrappable,
            parent: Cell::new(None),
            strong: Cell::new(true),
        })
    }
}

impl<R: Resource> From<&Rc<R>> for Weak<R> {
    fn from(r: &Rc<R>) -> Self {
        Self {
            weak: std::rc::Rc::downgrade(&r.handle),
            wrappable: Some(r.wrappable.as_ptr()),
        }
    }
}

impl<R: Resource> Clone for Weak<R> {
    fn clone(&self) -> Self {
        Self {
            weak: std::rc::Weak::clone(&self.weak),
            wrappable: self.wrappable,
        }
    }
}

impl<R: Resource> GarbageCollected for Weak<R> {
    /// No-op: weak references don't keep the target alive and have no GC edges to trace.
    fn trace(&self, _visitor: &mut v8::GcVisitor) {}

    fn memory_name(&self) -> &'static std::ffi::CStr {
        // jsgGetMemoryName is only called on live Wrappables, never on Weak<R>.
        // Delegate to the concrete R via a live upgrade. In the (unreachable)
        // case where the referent is already gone, return an empty string.
        self.weak.upgrade().as_deref().map_or(c"", R::memory_name)
    }
}

impl<R: Resource> Default for Weak<R> {
    fn default() -> Self {
        Self {
            weak: std::rc::Weak::new(),
            wrappable: None,
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
/// let a = resource.clone().to_js(lock);
/// let b = resource.to_js(lock);
/// // a === b in JavaScript (same object identity)
/// ```
///
/// **Prototype**: The returned object's prototype is set up from the resource's
/// [`FunctionTemplate`], which includes all `#[jsg_method]` instance methods and
/// `#[jsg_static_constant]` values registered via [`Resource::members()`].
///
/// **Ownership**: The `Rc` is consumed. The JS wrapper keeps the `Wrappable`
/// alive via `CppgcShim`. When the JS object is garbage collected and all Rust
/// `Rc`s are dropped, the resource is destroyed.
#[expect(clippy::needless_pass_by_value)]
pub fn wrap<'a, R: Resource + 'static>(
    lock: &mut Lock,
    resource: Rc<R>,
) -> v8::Local<'a, v8::Value> {
    let isolate = lock.isolate();
    let constructor = lock.realm().resources.get_constructor::<R>(isolate);

    resource.wrappable.to_js(isolate, constructor)
}

/// Returns the constructor function for a resource type as a `Local<Function>`.
///
/// Lazily creates and caches the `FunctionTemplate` in the `Realm` on first call.
/// The returned function can be exposed as a global (e.g., `ctx.set_global("MyClass", func)`).
pub fn function_template_of<'a, R: Resource + 'static>(
    lock: &mut Lock,
) -> v8::Local<'a, v8::Function> {
    let isolate = lock.isolate();
    let constructor = lock.realm().resources.get_constructor::<R>(isolate);
    // SAFETY: `isolate` is valid and locked (Lock invariant). `constructor` is a valid
    // Global<FunctionTemplate> cached in the Realm. Inlined from `as_local_function` to
    // avoid re-borrowing `self` while `constructor` holds an immutable borrow through `realm()`.
    unsafe {
        v8::Local::from_ffi(
            isolate,
            v8::ffi::function_template_get_function(isolate.as_ffi(), constructor.as_ffi_ref()),
        )
    }
}

/// Builds a [`ResourceDescriptor`] from `R`'s [`Resource::members()`] list.
/// The descriptor is passed to C++ `create_resource_template` to set up the
/// V8 `FunctionTemplate` with the correct methods, constants, and constructor.
fn get_resource_descriptor<R: Resource>() -> v8::ffi::ResourceDescriptor {
    let mut descriptor = v8::ffi::ResourceDescriptor {
        name: R::class_name().to_owned(),
        constructor: KjMaybe::None,
        methods: Vec::new(),
        static_methods: Vec::new(),
        static_constants: Vec::new(),
    };

    for m in R::members() {
        match m {
            Member::Constructor { callback } => {
                descriptor.constructor = KjMaybe::Some(v8::ffi::ConstructorDescriptor {
                    callback: callback as usize,
                });
            }
            Member::Method { name, callback } => {
                descriptor.methods.push(v8::ffi::MethodDescriptor {
                    name,
                    callback: callback as usize,
                });
            }
            Member::Property {
                name: _,
                getter_callback: _,
                setter_callback: _,
            } => unimplemented!("#[jsg_property] is not yet supported on Rust resources"),
            Member::StaticMethod { name, callback } => {
                descriptor.static_methods.push(v8::ffi::MethodDescriptor {
                    name,
                    callback: callback as usize,
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
        }
    }

    descriptor
}

#[derive(Default)]
pub struct Resources {
    /// Cached V8 `FunctionTemplate`s keyed by resource `TypeId`.
    templates: HashMap<TypeId, v8::Global<v8::FunctionTemplate>>,
}

impl Resources {
    /// Gets or creates the cached `FunctionTemplate` for a resource type.
    pub fn get_constructor<R: Resource + 'static>(
        &mut self,
        isolate: v8::IsolatePtr,
    ) -> &v8::Global<v8::FunctionTemplate> {
        self.templates
            .entry(TypeId::of::<R>())
            .or_insert_with(|| Self::create_resource_constructor::<R>(isolate))
    }

    /// Creates a new V8 `FunctionTemplate` for resource type `R` via the C++ FFI.
    fn create_resource_constructor<R: Resource>(
        isolate: v8::IsolatePtr,
    ) -> v8::Global<v8::FunctionTemplate> {
        // SAFETY: Caller guarantees the isolate is valid and locked.
        unsafe {
            v8::ffi::create_resource_template(isolate.as_ffi(), &get_resource_descriptor::<R>())
                .into()
        }
    }
}

/// Rust types exposed to JavaScript as resource types.
///
/// Resource types are passed by reference and call back into Rust when JavaScript accesses
/// their members. This is analogous to `JSG_RESOURCE_TYPE` in C++ JSG. Resources must provide
/// member declarations, a cleanup function for GC, and access to their V8 wrapper state.
pub trait Resource: Type + GarbageCollected + Sized + 'static {
    /// Returns the list of methods, properties, and constructors exposed to JavaScript.
    fn members() -> Vec<Member>
    where
        Self: Sized;
}
