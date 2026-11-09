// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "reports to the C++ `jsg::MemoryTracker` through the FFI"
)]

//! Heap snapshots: what Rust values report to C++ `jsg::MemoryTracker` (`workerd/jsg/memory.h`).
//!
//! - [`MemoryInfo`]: how a value is reported as a field (C++ `MemoryTracker::trackField`).
//! - [`MemoryRetainer`]: a type that is a node of its own (C++ `JSG_MEMORY_INFO`).
//! - [`VisitForMemoryInfo`]: a resource's fields (C++ `visitForMemoryInfo`).
//!
//! Every resource is a node sized as the resource plus its `Wrappable`, with an edge to its
//! wrapper, whether it reports fields or not. Names are `&'static CStr`: V8 reads them after the
//! tracker's callback returns.
//!
//! # Sizes
//!
//! A resource or retainer counts `size_of_val(self)`, its fields' inline bytes included, so a
//! retainer held inline in another is counted in both, as in C++. Strings and collections count
//! what they allocate; a collection counts its element slots only, a low estimate. A collection or
//! `Box` leaves the bytes of retainers it holds inline to their nodes
//! ([`MemoryInfo::self_bytes_reported`]).

use std::any::TypeId;
use std::ffi::CStr;
use std::ffi::c_char;
use std::hash::DefaultHasher;
use std::hash::Hash;
use std::hash::Hasher;
use std::ops::Deref;
use std::ops::DerefMut;
use std::pin::Pin;

use crate::Dict;
use crate::Lenient;
use crate::NonCoercible;
use crate::NullOrUndefined;
use crate::Nullable;
use crate::isolate_state::IsolateState;
use crate::v8::ffi;

/// The C++ `jsg::MemoryTracker` of a heap snapshot in progress, to which a value reports what
/// it retains. A heap snapshot is taken under the isolate lock, outside any `IoContext`.
pub struct MemoryTracker<'a> {
    inner: Pin<&'a mut ffi::MemoryTracker>,
    /// The isolate whose heap the snapshot is of.
    isolate: *mut ffi::Isolate,
}

fn name_ptr(name: Option<&'static CStr>) -> *const c_char {
    name.map_or(std::ptr::null(), CStr::as_ptr)
}

/// Tells values of type `R` from others at the same address: a hash of its `TypeId`. A collision
/// between two types at one address would merge their nodes.
#[expect(
    clippy::cast_possible_truncation,
    reason = "a tag needs no more bits than a pointer has"
)]
fn type_tag<R: ?Sized + 'static>() -> usize {
    let mut hasher = DefaultHasher::new();
    TypeId::of::<R>().hash(&mut hasher);
    hasher.finish() as usize
}

/// Pops the pushed node on drop, so the node stack stays balanced however reporting ends.
struct PushedNode<'t, 'a> {
    tracker: &'t mut MemoryTracker<'a>,
}

impl<'a> Deref for PushedNode<'_, 'a> {
    type Target = MemoryTracker<'a>;

    fn deref(&self) -> &Self::Target {
        self.tracker
    }
}

impl DerefMut for PushedNode<'_, '_> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.tracker
    }
}

impl Drop for PushedNode<'_, '_> {
    fn drop(&mut self) {
        // SAFETY: pops the node that the push that made this guard pushed.
        unsafe { ffi::memory_tracker_pop_node(self.tracker.inner.as_mut()) };
    }
}

impl<'a> MemoryTracker<'a> {
    /// How deep nodes nest, which bounds the stack a long chain uses. A node that would be this
    /// deep is a leaf with nothing inside it (a resource with a wrapper: an edge to the wrapper),
    /// and is not deduplicated, so a shallower path still reports the value in full.
    pub const MAX_DEPTH: usize = 128;

    pub(crate) fn from_ffi(mut inner: Pin<&'a mut ffi::MemoryTracker>) -> Self {
        // SAFETY: the tracker is valid for the duration of the snapshot.
        let isolate = unsafe { ffi::memory_tracker_isolate(inner.as_mut()) };
        Self { inner, isolate }
    }

    /// The C++ `jsg::MemoryTracker`, for a [`MemoryInfo`] impl in another crate that reports a
    /// value Rust JSG doesn't know, such as a C++ JSG `jsg::Ref`, through its own FFI.
    pub fn as_ffi(&mut self) -> Pin<&mut ffi::MemoryTracker> {
        self.inner.as_mut()
    }

    /// Whether a node pushed now would be [`Self::MAX_DEPTH`] deep, so that it is a leaf.
    fn is_too_deep(&self) -> bool {
        // SAFETY: the tracker is valid for the duration of the snapshot.
        unsafe { ffi::memory_tracker_depth(&self.inner) + 1 >= Self::MAX_DEPTH }
    }

    /// Reports `value`, a field of the object being tracked, under the edge `edge_name`: C++
    /// `MemoryTracker::trackField`.
    pub fn track_field<T: MemoryInfo + ?Sized>(&mut self, edge_name: &'static CStr, value: &T) {
        value.track_as_field(self, Some(edge_name));
    }

    /// Adds a node of `size` bytes named `node_name`, or `edge_name` if it has none, with an
    /// edge to it from the object being tracked; nothing if `size` is 0. C++
    /// `MemoryTracker::trackFieldWithSize`. An `edge_name` of `None` is the empty name of a
    /// collection's element.
    pub fn track_field_with_size(
        &mut self,
        edge_name: Option<&'static CStr>,
        size: usize,
        node_name: Option<&'static CStr>,
    ) {
        // SAFETY: the names are literals or null.
        unsafe {
            ffi::memory_tracker_track_field_with_size(
                self.inner.as_mut(),
                name_ptr(edge_name),
                size,
                name_ptr(node_name),
            );
        }
    }

    /// Reports `retainer` as a node of its own the first time; later reports of the same address
    /// and type add only an edge. A zero-sized retainer is a new node each time.
    ///
    /// Report fields, not values computed from them: two temporaries in one stack slot would be
    /// taken for one retainer.
    pub fn track_retainer<R: MemoryRetainer + ?Sized + 'static>(
        &mut self,
        edge_name: Option<&'static CStr>,
        retainer: &R,
    ) {
        let address = if size_of_val(retainer) == 0 {
            0
        } else {
            std::ptr::from_ref(retainer).cast::<()>() as usize
        };
        self.track_deduplicated(
            edge_name,
            (address, type_tag::<R>()),
            retainer.memory_name(),
            retainer.memory_self_size(),
            |tracker| retainer.memory_info(tracker),
        );
    }

    /// Pushes a node keyed by `key`, an address and a type tag (address 0 is never deduplicated),
    /// and has `contents` report into it, or adds only an edge if the key was seen.
    fn track_deduplicated(
        &mut self,
        edge_name: Option<&'static CStr>,
        key: (usize, usize),
        name: &'static CStr,
        size: usize,
        contents: impl FnOnce(&mut Self),
    ) {
        if self.is_too_deep() {
            self.track_leaf(edge_name, name, size);
            return;
        }
        let (address, type_tag) = key;
        // SAFETY: the names are literals or null.
        let pushed = unsafe {
            ffi::memory_tracker_push_retainer(
                self.inner.as_mut(),
                address,
                type_tag,
                name.as_ptr(),
                size,
                name_ptr(edge_name),
            )
        };
        if pushed {
            contents(&mut PushedNode { tracker: self });
        }
    }

    /// Adds a node with nothing inside it; unlike [`Self::track_field_with_size`], also of size 0.
    fn track_leaf(&mut self, edge_name: Option<&'static CStr>, name: &'static CStr, size: usize) {
        // SAFETY: the names are literals or null.
        unsafe {
            ffi::memory_tracker_push_container(
                self.inner.as_mut(),
                name.as_ptr(),
                size,
                name_ptr(edge_name),
            );
        }
        drop(PushedNode { tracker: self });
    }

    /// Pushes a node for a container and has `elements` report into it, with `None` edge names.
    pub fn track_container(
        &mut self,
        edge_name: Option<&'static CStr>,
        name: &'static CStr,
        size: usize,
        elements: impl FnOnce(&mut Self),
    ) {
        if self.is_too_deep() {
            self.track_leaf(edge_name, name, size);
            return;
        }
        // SAFETY: the names are literals or null.
        unsafe {
            ffi::memory_tracker_push_container(
                self.inner.as_mut(),
                name.as_ptr(),
                size,
                name_ptr(edge_name),
            );
        }
        elements(&mut PushedNode { tracker: self });
    }

    /// Reports a resource of the snapshot's isolate, as C++ reports a `jsg::Ref` field.
    pub(crate) fn track_wrappable(
        &mut self,
        edge_name: Option<&'static CStr>,
        wrappable: &ffi::Wrappable,
    ) {
        if self.is_too_deep() {
            // SAFETY: the name is a literal or null.
            unsafe {
                ffi::memory_tracker_track_wrappable_leaf(
                    self.inner.as_mut(),
                    name_ptr(edge_name),
                    wrappable,
                );
            }
            return;
        }
        // SAFETY: the name is a literal or null.
        unsafe {
            ffi::memory_tracker_track_wrappable(
                self.inner.as_mut(),
                name_ptr(edge_name),
                wrappable,
            );
        }
    }

    /// Whether a handle or resource of `state`'s isolate can be reported: it is the snapshot's
    /// isolate, and not being disposed.
    pub(crate) fn can_track_object_of(&self, state: &IsolateState) -> bool {
        state.belongs_to(self.isolate) && !state.is_closed()
    }

    /// Adds an edge to the value of `global`, unless it is empty.
    ///
    /// # Safety
    ///
    /// `global` is a handle of the tracker's isolate, or empty; see
    /// [`can_track_object_of`](Self::can_track_object_of).
    pub(crate) unsafe fn track_global(
        &mut self,
        edge_name: Option<&'static CStr>,
        global: &ffi::Global,
    ) {
        // SAFETY: the name is a literal or null; the caller guarantees the handle's isolate.
        unsafe {
            ffi::memory_tracker_track_global(self.inner.as_mut(), name_ptr(edge_name), global);
        }
    }
}

/// How a value is reported when it is a field of a tracked object (C++ `trackField`).
///
/// # Rules for reporting code
///
/// [`VisitForMemoryInfo::visit_for_memory_info`], [`MemoryRetainer::memory_info`] and every impl
/// of this trait:
///
/// - must not panic: it runs in a callback from C++, where a panic aborts the process;
/// - must not write into anything it can reach. A [`crate::Cell`] is reported in place, and
///   writes into any `jsg::Cell` on the thread panic meanwhile; a `RefCell` is borrowed, so
///   `borrow_mut` panics;
/// - must not call into JavaScript, and should allocate nothing.
///
/// # Built-in implementations
///
/// Primitives report nothing; strings, buffers and collections a node of what they allocate;
/// wrappers (`Option`, `RefCell`, `jsg::Cell`, ...) what they hold; handles, promises and frames
/// an edge into the V8 heap; `jsg::Rc` the resource's node. Less obviously:
///
/// - A collection of [plain data](Self::PLAIN_DATA) is one node; an empty collection is none.
/// - `Box`, `Rc` and `Arc` report a retainer as its node, plain data as a node of its size, and
///   anything else as a node of its uncounted bytes. An allocation shared by several `Rc`s or
///   `Arc`s is one node, which keeps the work linear and ends cycles.
/// - A mutably borrowed `RefCell` reports nothing.
/// - The impl for `std::cell::Cell<T>` exists only for `T: Copy` and reports nothing; hold a
///   retainer in a `jsg::Cell`.
/// - `jsg::Weak`, the root async context frame, and a handle or `jsg::Rc` of another isolate
///   report nothing.
pub trait MemoryInfo {
    /// Whether the type retains nothing beyond its own bytes, so a collection of it is one node.
    const PLAIN_DATA: bool = false;

    /// How many of `self`'s own bytes the nodes it reports already count, which the collection or
    /// `Box` holding it leaves out. A [`MemoryRetainer`] returns its own bytes; an inline wrapper
    /// (`Option`, `Cell`, ...) forwards its value's; anything else returns 0.
    fn self_bytes_reported(&self) -> usize {
        0
    }

    /// Reports `self` under the edge `edge_name`; `None` is a collection element's empty name.
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>);
}

/// A type that is a node of its own in a heap snapshot.
///
/// C++ `JSG_MEMORY_INFO`. [`jsg_memory_retainer!`](crate::jsg_memory_retainer) gives it its
/// [`MemoryInfo`] impl.
///
/// ```ignore
/// struct Cache {
///     entries: HashMap<String, String>,
/// }
///
/// impl jsg::MemoryRetainer for Cache {
///     fn memory_name(&self) -> &'static CStr {
///         c"Cache"
///     }
///
///     fn memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
///         tracker.track_field(c"entries", &self.entries);
///     }
/// }
///
/// jsg::jsg_memory_retainer!(Cache);
/// ```
pub trait MemoryRetainer {
    /// The name of the node, which the snapshot shows as `workerd / <name>`.
    fn memory_name(&self) -> &'static CStr;

    /// The size of the node: the value's own bytes, without what it allocates.
    fn memory_self_size(&self) -> usize {
        std::mem::size_of_val(self)
    }

    /// Reports the fields, following the [rules](MemoryInfo#rules-for-reporting-code).
    fn memory_info(&self, tracker: &mut MemoryTracker<'_>);
}

/// Implements [`MemoryInfo`] for `'static` [`MemoryRetainer`] types:
/// `jsg::jsg_memory_retainer!(Cache, Entry);`. A generic type writes the same impl by hand.
#[macro_export]
macro_rules! jsg_memory_retainer {
    ($($t:ty),* $(,)?) => {
        $(impl $crate::MemoryInfo for $t {
            fn self_bytes_reported(&self) -> usize {
                ::std::cmp::min(
                    ::std::mem::size_of_val(self),
                    $crate::MemoryRetainer::memory_self_size(self),
                )
            }

            fn track_as_field(
                &self,
                tracker: &mut $crate::MemoryTracker<'_>,
                edge_name: ::std::option::Option<&'static ::std::ffi::CStr>,
            ) {
                tracker.track_retainer(edge_name, self);
            }
        })*
    };
}

/// The fields a resource reports (C++ `visitForMemoryInfo`).
///
/// `#[jsg_resource(memory_info)]` generates it from the named fields, so a new field is reported,
/// or fails to compile without [`MemoryInfo`]; `#[jsg_memory_info(skip)]` leaves one out.
/// `#[jsg_resource(custom_memory_info)]` takes a hand-written impl:
///
/// ```ignore
/// impl jsg::VisitForMemoryInfo for Foo {
///     fn visit_for_memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
///         tracker.track_field(c"name", &self.name);
///     }
/// }
/// ```
///
/// Without either flag a resource reports no fields.
pub trait VisitForMemoryInfo {
    /// Reports the fields, following the [rules](MemoryInfo#rules-for-reporting-code).
    fn visit_for_memory_info(&self, tracker: &mut MemoryTracker<'_>);
}

// =============================================================================
// Built-in implementations
// =============================================================================

/// Generates `MemoryInfo` impls for plain data, which reports nothing.
macro_rules! impl_memory_info_plain {
    ($($t:ty),* $(,)?) => {
        $(impl MemoryInfo for $t {
            const PLAIN_DATA: bool = true;

            fn track_as_field(&self, _: &mut MemoryTracker<'_>, _: Option<&'static CStr>) {}
        })*
    };
}

impl_memory_info_plain!(
    (),
    bool,
    char,
    u8,
    u16,
    u32,
    u64,
    u128,
    usize,
    i8,
    i16,
    i32,
    i64,
    i128,
    isize,
    f32,
    f64,
    std::time::SystemTime,
    std::time::Duration,
    &str,
);

impl MemoryInfo for String {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        tracker.track_field_with_size(edge_name, self.capacity(), Some(c"String"));
    }
}

impl MemoryInfo for Box<str> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        tracker.track_field_with_size(edge_name, self.len(), Some(c"Box<str>"));
    }
}

impl MemoryInfo for crate::v8::BackingStore {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        tracker.track_field_with_size(edge_name, self.byte_length(), Some(c"BackingStore"));
    }
}

impl<T: MemoryInfo> MemoryInfo for Option<T> {
    const PLAIN_DATA: bool = T::PLAIN_DATA;

    fn self_bytes_reported(&self) -> usize {
        self.as_ref().map_or(0, T::self_bytes_reported)
    }

    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        if let Some(inner) = self {
            inner.track_as_field(tracker, edge_name);
        }
    }
}

impl<T: MemoryInfo> MemoryInfo for Nullable<T> {
    const PLAIN_DATA: bool = T::PLAIN_DATA;

    fn self_bytes_reported(&self) -> usize {
        match self {
            Self::Some(inner) => inner.self_bytes_reported(),
            Self::Null => 0,
        }
    }

    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        if let Self::Some(inner) = self {
            inner.track_as_field(tracker, edge_name);
        }
    }
}

impl<T: MemoryInfo> MemoryInfo for NullOrUndefined<T> {
    const PLAIN_DATA: bool = T::PLAIN_DATA;

    fn self_bytes_reported(&self) -> usize {
        match self {
            Self::Some(inner) => inner.self_bytes_reported(),
            Self::Null | Self::Undefined => 0,
        }
    }

    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        if let Self::Some(inner) = self {
            inner.track_as_field(tracker, edge_name);
        }
    }
}

// Not plain data even when `T` is: `Unconvertable` holds a `TracedGlobal`, an edge into V8.
impl<T: MemoryInfo> MemoryInfo for Lenient<T> {
    fn self_bytes_reported(&self) -> usize {
        match self {
            Self::Some(inner) => inner.self_bytes_reported(),
            Self::Unconvertable(inner) => inner.self_bytes_reported(),
            Self::Null | Self::Undefined => 0,
        }
    }

    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        match self {
            Self::Some(inner) => inner.track_as_field(tracker, edge_name),
            Self::Unconvertable(inner) => inner.track_as_field(tracker, edge_name),
            Self::Null | Self::Undefined => {}
        }
    }
}

impl<T: MemoryInfo> MemoryInfo for NonCoercible<T> {
    const PLAIN_DATA: bool = T::PLAIN_DATA;

    fn self_bytes_reported(&self) -> usize {
        self.as_ref().self_bytes_reported()
    }

    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        self.as_ref().track_as_field(tracker, edge_name);
    }
}

/// Plain data. Reporting the `T` in place would be unsound (a write could drop it), and a copy
/// would put a retainer at the copy's address.
impl<T: Copy> MemoryInfo for std::cell::Cell<T> {
    const PLAIN_DATA: bool = true;

    fn track_as_field(&self, _: &mut MemoryTracker<'_>, _: Option<&'static CStr>) {}
}

impl<T: MemoryInfo> MemoryInfo for std::cell::RefCell<T> {
    const PLAIN_DATA: bool = T::PLAIN_DATA;

    fn self_bytes_reported(&self) -> usize {
        self.try_borrow()
            .map_or(0, |inner| inner.self_bytes_reported())
    }

    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        // A heap snapshot can start while code holds a mutable borrow, if that code takes one.
        // The contents cannot be read then.
        if let Ok(inner) = self.try_borrow() {
            inner.track_as_field(tracker, edge_name);
        }
    }
}

impl<T: MemoryInfo> MemoryInfo for Box<T> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        let inner = &**self;
        if T::PLAIN_DATA {
            tracker.track_field_with_size(edge_name, size_of::<T>(), Some(c"Box"));
            return;
        }
        let uncounted = size_of::<T>().saturating_sub(inner.self_bytes_reported());
        if uncounted == 0 {
            inner.track_as_field(tracker, edge_name);
        } else {
            tracker.track_container(edge_name, c"Box", uncounted, |tracker| {
                inner.track_as_field(tracker, None);
            });
        }
    }
}

/// Reports what an `Rc` or `Arc` shares as a `Box` would, except that the allocation's node is
/// deduplicated by `address` and the pointer kind `K`. A retainer deduplicates itself.
fn track_shared<K: 'static, T: MemoryInfo>(
    tracker: &mut MemoryTracker<'_>,
    edge_name: Option<&'static CStr>,
    name: &'static CStr,
    address: *const T,
    inner: &T,
) {
    let size = if T::PLAIN_DATA {
        size_of::<T>()
    } else {
        size_of::<T>().saturating_sub(inner.self_bytes_reported())
    };
    if size == 0 {
        inner.track_as_field(tracker, edge_name);
        return;
    }
    tracker.track_deduplicated(
        edge_name,
        (address as usize, type_tag::<K>()),
        name,
        size,
        |tracker| {
            if !T::PLAIN_DATA {
                inner.track_as_field(tracker, None);
            }
        },
    );
}

// The kind of a pointer, not its type, tags its allocation: one allocation holds one `T`.
impl<T: MemoryInfo> MemoryInfo for std::rc::Rc<T> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        track_shared::<std::rc::Rc<()>, T>(tracker, edge_name, c"Rc", Self::as_ptr(self), self);
    }
}

impl<T: MemoryInfo> MemoryInfo for std::sync::Arc<T> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        track_shared::<std::sync::Arc<()>, T>(tracker, edge_name, c"Arc", Self::as_ptr(self), self);
    }
}

impl<T: MemoryInfo, const N: usize> MemoryInfo for [T; N] {
    const PLAIN_DATA: bool = T::PLAIN_DATA;

    fn self_bytes_reported(&self) -> usize {
        self.iter().map(T::self_bytes_reported).sum()
    }

    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        // The elements are inline, in the object being tracked, so each is a field of it.
        for element in self {
            element.track_as_field(tracker, edge_name);
        }
    }
}

/// Reports a collection of `len` elements of type `T` in an allocation of `size` bytes.
fn track_collection<'e, T: MemoryInfo + 'e>(
    tracker: &mut MemoryTracker<'_>,
    edge_name: Option<&'static CStr>,
    name: &'static CStr,
    size: usize,
    len: usize,
    elements: impl IntoIterator<Item = &'e T> + Clone,
) {
    if len == 0 {
        return;
    }
    if T::PLAIN_DATA {
        tracker.track_field_with_size(edge_name, size, Some(name));
        return;
    }
    // The elements' nodes count some of the elements' bytes, such as a retainer's.
    let reported: usize = elements
        .clone()
        .into_iter()
        .map(T::self_bytes_reported)
        .sum();
    tracker.track_container(edge_name, name, size.saturating_sub(reported), |tracker| {
        for element in elements {
            element.track_as_field(tracker, None);
        }
    });
}

/// Reports a map of `len` entries of types `K` and `V` in an allocation of `size` bytes.
fn track_map<'e, K: MemoryInfo + 'e, V: MemoryInfo + 'e>(
    tracker: &mut MemoryTracker<'_>,
    edge_name: Option<&'static CStr>,
    name: &'static CStr,
    size: usize,
    len: usize,
    entries: impl IntoIterator<Item = (&'e K, &'e V)> + Clone,
) {
    if len == 0 {
        return;
    }
    if K::PLAIN_DATA && V::PLAIN_DATA {
        tracker.track_field_with_size(edge_name, size, Some(name));
        return;
    }
    let reported: usize = entries
        .clone()
        .into_iter()
        .map(|(key, value)| key.self_bytes_reported() + value.self_bytes_reported())
        .sum();
    tracker.track_container(edge_name, name, size.saturating_sub(reported), |tracker| {
        for (key, value) in entries {
            key.track_as_field(tracker, Some(c"key"));
            value.track_as_field(tracker, Some(c"value"));
        }
    });
}

impl<T: MemoryInfo> MemoryInfo for Vec<T> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        let size = self.capacity() * size_of::<T>();
        track_collection(tracker, edge_name, c"Vec", size, self.len(), self);
    }
}

impl<T: MemoryInfo> MemoryInfo for Box<[T]> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        let size = self.len() * size_of::<T>();
        track_collection(
            tracker,
            edge_name,
            c"Box<[T]>",
            size,
            self.len(),
            self.iter(),
        );
    }
}

impl<T: MemoryInfo> MemoryInfo for std::collections::VecDeque<T> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        let size = self.capacity() * size_of::<T>();
        track_collection(tracker, edge_name, c"VecDeque", size, self.len(), self);
    }
}

impl<V: MemoryInfo> MemoryInfo for Dict<V> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        let entries = self.entries();
        if entries.is_empty() {
            return;
        }
        let size = entries.capacity() * size_of::<(String, V)>();
        if V::PLAIN_DATA {
            // The keys are the only other memory, so the whole is one node.
            let keys: usize = entries.iter().map(|(key, _)| key.capacity()).sum();
            tracker.track_field_with_size(edge_name, size + keys, Some(c"Dict"));
            return;
        }
        let reported: usize = entries
            .iter()
            .map(|(_, value)| value.self_bytes_reported())
            .sum();
        let size = size.saturating_sub(reported);
        tracker.track_container(edge_name, c"Dict", size, |tracker| {
            for (key, value) in entries {
                key.track_as_field(tracker, Some(c"key"));
                value.track_as_field(tracker, Some(c"value"));
            }
        });
    }
}

impl<K: MemoryInfo, V: MemoryInfo, S: std::hash::BuildHasher> MemoryInfo
    for std::collections::HashMap<K, V, S>
{
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        let size = self.capacity() * size_of::<(K, V)>();
        track_map(tracker, edge_name, c"HashMap", size, self.len(), self);
    }
}

impl<K: MemoryInfo, V: MemoryInfo> MemoryInfo for std::collections::BTreeMap<K, V> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        let size = self.len() * size_of::<(K, V)>();
        track_map(tracker, edge_name, c"BTreeMap", size, self.len(), self);
    }
}

impl<T: MemoryInfo, S: std::hash::BuildHasher> MemoryInfo for std::collections::HashSet<T, S> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        let size = self.capacity() * size_of::<T>();
        track_collection(tracker, edge_name, c"HashSet", size, self.len(), self);
    }
}

impl<T: MemoryInfo> MemoryInfo for std::collections::BTreeSet<T> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        let size = self.len() * size_of::<T>();
        track_collection(tracker, edge_name, c"BTreeSet", size, self.len(), self);
    }
}
