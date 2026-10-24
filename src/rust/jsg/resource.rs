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
use std::fmt;
use std::ops::Deref;
use std::ptr::NonNull;
use std::sync::Arc;

use kj_rs::KjMaybe;

use crate::ConstantValue;
use crate::Error;
use crate::FromJS;
use crate::GarbageCollected;
use crate::Lock;
use crate::Member;
use crate::ToJS;
use crate::Traced;
use crate::Type;
use crate::feature_flags::compatibility_flags;
use crate::isolate_state::IsolateState;
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
    pub fn attach_to_this(&self, info: &mut v8::FunctionCallbackInfo<'_>) {
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

impl<'s, R: Resource + 'static> ToJS<'s> for Rc<R> {
    fn to_js(self, lock: &mut Lock<'s>) -> v8::Local<'s, v8::Value> {
        wrap(lock, self)
    }
}

impl<'s, R: Resource + 'static> FromJS<'s> for Rc<R> {
    type ResultType = Self;

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
        let Some(mut wrappable) = v8::WrappableRc::from_js(lock, value) else {
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
        Ok(Some(Self {
            handle,
            wrappable,
            parent: Cell::new(None),
            strong: Cell::new(true),
        }))
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

/// `Rc<R>` is a strong GC edge — visited via `GcVisitor::visit_rc`.
impl<R: Resource> Traced for Rc<R> {
    fn trace(&self, visitor: &mut v8::GcVisitor) {
        visitor.visit_rc(self);
    }
}

/// Weak references don't keep the target alive and have no GC edges to trace.
impl<R: Resource> Traced for Weak<R> {}

impl<R: Resource> GarbageCollected for Weak<R> {
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
/// `#[jsg_static_constant]` values registered via [`Resource::members(flags)`](Resource::members).
///
/// **Ownership**: The `Rc` is consumed. The JS wrapper keeps the `Wrappable`
/// alive via `CppgcShim`. When the JS object is garbage collected and all Rust
/// `Rc`s are dropped, the resource is destroyed.
///
/// # Panics
/// Panics if the resource already has a JavaScript wrapper in another isolate. The `Rc` is
/// leaked in that case.
#[expect(
    clippy::panic,
    reason = "using a wrapper in another isolate would mix the isolates' heaps"
)]
pub fn wrap<'s, R: Resource + 'static>(
    lock: &mut Lock<'s>,
    resource: Rc<R>,
) -> v8::Local<'s, v8::Value> {
    if let Some(wrapper) = resource.wrappable.to_js::<R>(lock) {
        return wrapper;
    }
    // Dropping `resource` here would release its strong reference on a wrapper of another
    // isolate, which needs that isolate's lock. It is leaked in a box instead, which keeps
    // pointers to the resource and its Wrappable for LeakSanitizer to follow.
    let leaked = Box::leak(Box::new(resource));
    // SAFETY: `leaked` is a live heap allocation.
    unsafe { v8::ffi::ignore_leak(std::ptr::from_mut(leaked).expose_provenance()) };
    panic!("attempt to wrap a resource in an isolate other than the one it was wrapped in");
}

/// Returns the constructor function for a resource type as a `Local<Function>`.
///
/// Lazily creates and caches the `FunctionTemplate` in the `Realm` on first call.
/// The returned function can be exposed as a global (e.g., `ctx.set_global("MyClass", func)`).
pub fn function_template_of<'s, R: Resource + 'static>(
    lock: &mut Lock<'s>,
) -> v8::Local<'s, v8::Function> {
    let constructor = lock.realm().get_constructor::<R>().as_ffi();
    // SAFETY: the constructor is a function template of the lock's isolate, which the Realm's
    // template cache keeps alive.
    unsafe { v8::function_template_get_function(lock, &constructor) }
}

/// Builds a [`ResourceDescriptor`] from `R`'s [`Resource::members(flags)`](Resource::members) list.
/// The descriptor is passed to C++ `create_resource_template` to set up the
/// V8 `FunctionTemplate` with the correct methods, constants, and constructor.
/// Nested types are returned separately, since their templates are installed
/// once the template for `R` exists.
fn get_resource_descriptor<R: Resource>(
    flags: compatibility_flags::Reader<'_>,
) -> (v8::ffi::ResourceDescriptor, NestedTypes) {
    let mut nested_types = Vec::new();
    let mut descriptor = v8::ffi::ResourceDescriptor {
        name: R::class_name().to_owned(),
        constructor: KjMaybe::None,
        methods: Vec::new(),
        properties: Vec::new(),
        static_methods: Vec::new(),
        static_constants: Vec::new(),
    };

    for m in R::members(flags) {
        match m {
            Member::Constructor { callback } => {
                descriptor.constructor = KjMaybe::Some(v8::ffi::ConstructorDescriptor {
                    callback: callback as usize,
                });
            }
            Member::Method {
                name,
                callback,
                fast,
            } => {
                descriptor.methods.push(v8::ffi::MethodDescriptor {
                    name,
                    callback: callback as usize,
                    fast: fast.into(),
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
            Member::StaticMethod {
                name,
                callback,
                fast,
            } => {
                descriptor.static_methods.push(v8::ffi::MethodDescriptor {
                    name,
                    callback: callback as usize,
                    fast: fast.into(),
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
            Member::NestedType { name, nested_type } => nested_types.push((name, nested_type)),
        }
    }

    (descriptor, nested_types)
}

/// The nested types of a resource type, with their names.
type NestedTypes = Vec<(String, NestedType)>;

/// The resource types whose templates are being created, each nesting the next,
/// with their Rust type names.
type Nesting = Vec<(TypeId, &'static str)>;

/// A resource type that another resource type exposes as a nested type
/// ([`Member::NestedType`]).
///
/// A resource type must not nest itself, directly or through other nested
/// types; creating its template panics.
pub struct NestedType {
    template: for<'a, 'f> fn(
        &'a mut Resources,
        v8::IsolatePtr,
        &Arc<IsolateState>,
        compatibility_flags::Reader<'f>,
        &mut Nesting,
    ) -> &'a v8::Global<v8::FunctionTemplate>,
}

impl NestedType {
    /// The nested type `R`.
    #[must_use]
    pub fn of<R: Resource>() -> Self {
        Self {
            template: Resources::get_nested_constructor::<R>,
        }
    }
}

#[derive(Default)]
pub struct Resources {
    /// Cached V8 `FunctionTemplate`s keyed by resource `TypeId`, which GC never visits.
    templates: HashMap<TypeId, v8::Global<v8::FunctionTemplate>>,
}

impl Resources {
    /// Gets or creates the cached `FunctionTemplate` for a resource type.
    ///
    /// `flags` are only consulted when the template is first created.
    pub(crate) fn get_constructor<R: Resource + 'static>(
        &mut self,
        isolate: v8::IsolatePtr,
        state: &Arc<IsolateState>,
        flags: compatibility_flags::Reader<'_>,
    ) -> &v8::Global<v8::FunctionTemplate> {
        self.get_nested_constructor::<R>(isolate, state, flags, &mut Vec::new())
    }

    /// Gets or creates the cached `FunctionTemplate` for a resource type, while the
    /// templates of the types in `nesting` are being created, each nesting the next.
    ///
    /// # Panics
    ///
    /// Panics if `R`'s template is being created already, meaning that `R`
    /// directly or indirectly nests itself.
    #[expect(
        clippy::panic,
        reason = "a resource type nesting itself is a programming error"
    )]
    fn get_nested_constructor<'a, R: Resource + 'static>(
        &'a mut self,
        isolate: v8::IsolatePtr,
        state: &Arc<IsolateState>,
        flags: compatibility_flags::Reader<'_>,
        nesting: &mut Nesting,
    ) -> &'a v8::Global<v8::FunctionTemplate> {
        let type_id = TypeId::of::<R>();
        if !self.templates.contains_key(&type_id) {
            let name = std::any::type_name::<R>();
            if nesting.iter().any(|&(id, _)| id == type_id) {
                let path: Vec<_> = nesting.iter().map(|&(_, name)| name).collect();
                panic!(
                    "resource type nests itself: {} -> {name}",
                    path.join(" -> ")
                );
            }
            nesting.push((type_id, name));
            let template = self.create_resource_constructor::<R>(isolate, state, flags, nesting);
            nesting.pop();
            self.templates.insert(type_id, template);
        }
        &self.templates[&type_id]
    }

    /// Creates a new V8 `FunctionTemplate` for resource type `R` via the C++ FFI,
    /// creating the templates of its nested types as needed.
    fn create_resource_constructor<R: Resource>(
        &mut self,
        isolate: v8::IsolatePtr,
        state: &Arc<IsolateState>,
        flags: compatibility_flags::Reader<'_>,
        nesting: &mut Nesting,
    ) -> v8::Global<v8::FunctionTemplate> {
        let (descriptor, nested_types) = get_resource_descriptor::<R>(flags);
        // SAFETY: Caller guarantees the isolate is valid and locked.
        let handle = unsafe { v8::ffi::create_resource_template(isolate.as_ffi(), &descriptor) };
        let template = v8::Global::from_ffi_in(handle, Arc::clone(state));
        for (name, nested_type) in nested_types {
            let nested = (nested_type.template)(self, isolate, state, flags, nesting);
            // SAFETY: The caller guarantees the isolate is valid and locked, and both
            // templates are live `Global<FunctionTemplate>`s of this isolate. The template
            // for `R` has not been instantiated yet, so it can still be modified.
            unsafe {
                v8::ffi::function_template_set_nested_type(
                    isolate.as_ffi(),
                    &template.as_ffi(),
                    &name,
                    &nested.as_ffi(),
                );
            }
        }
        template
    }

    /// Drains every cached resource-template handle, transferring ownership of each
    /// `v8::Global<v8::FunctionTemplate>` to the caller. Used by the `PREPARE_SNAPSHOT`
    /// pipeline, which disposes each handle before `CreateBlob`; the cache is left empty and
    /// templates are recreated lazily on demand.
    pub fn take_template_handles(&mut self) -> Vec<v8::ffi::Global> {
        self.templates
            .drain()
            .map(|(_, global)| global.into_ffi_for_snapshot())
            .collect()
    }
}

/// Rust types exposed to JavaScript as resource types.
///
/// Resource types are passed by reference and call back into Rust when JavaScript accesses
/// their members. This is analogous to `JSG_RESOURCE_TYPE` in C++ JSG. Resources must provide
/// member declarations, a cleanup function for GC, and access to their V8 wrapper state.
pub trait Resource: Type + GarbageCollected + Sized + 'static {
    /// Returns the list of methods, properties, and constructors exposed to JavaScript.
    ///
    /// `flags` are the compatibility flags of the isolate the template is built for, so the
    /// member list can vary with them, as with the `CompatibilityFlags::Reader flags`
    /// parameter of C++ `JSG_RESOURCE_TYPE`. Templates are cached per resource type in the realm.
    /// Use `#[jsg_resource(custom_members)]` to generate member factories while supplying
    /// this implementation yourself.
    fn members(flags: compatibility_flags::Reader<'_>) -> Vec<Member>
    where
        Self: Sized;
}

#[cfg(test)]
#[path = "resource-test.rs"]
mod tests;
