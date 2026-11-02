// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "reports to the C++ `jsg::MemoryTracker` through the FFI"
)]

//! Heap snapshot support: what Rust values report to C++ `jsg::MemoryTracker`
//! (`workerd/jsg/memory.h`), which adds them to the graph of embedder objects in a V8 heap
//! snapshot.
//!
//! - [`MemoryTracker`] is the tracker, as C++ code receives it in `jsgGetMemoryInfo`.
//! - [`MemoryInfo`] says how a value is reported when it is a field of a tracked object, as the
//!   overloads of C++ `MemoryTracker::trackField` do.
//! - [`MemoryRetainer`] makes a type a node of its own, as C++ `JSG_MEMORY_INFO` does;
//!   [`jsg_memory_retainer!`](crate::jsg_memory_retainer) gives it its `MemoryInfo` impl.
//! - [`VisitForMemoryInfo`] reports the fields of a resource, as C++ `visitForMemoryInfo` does.
//!   `#[jsg_resource(memory_info)]` on the struct opts the resource in.
//!
//! Every resource is a node, named after its type and sized as the resource plus its C++
//! `Wrappable`, with an edge to its JavaScript wrapper if it has one, whether it reports its
//! fields or not.
//!
//! Names are `&'static CStr`, written as `c"..."` literals: V8 reads the names of nodes and
//! edges after the tracker's callback returns.
//!
//! The code that reports values follows the rules in
//! [`MemoryInfo`](MemoryInfo#rules-for-reporting-code).
//!
//! # Sizes
//!
//! A node's size is the bytes it accounts for. A resource or retainer counts its own bytes
//! (`size_of_val`), which include its fields' inline bytes, and a string or collection counts the
//! memory it allocates. A retainer held inline in another, as a field, is therefore counted in
//! both nodes, as a C++ member with `JSG_MEMORY_INFO` is. A collection leaves the bytes of the
//! retainers it holds inline, directly or in an `Option`, `Cell` or other inline wrapper, to their
//! own nodes (see [`MemoryInfo::self_bytes_reported`]). A value shared through an `std::rc::Rc`
//! or `std::sync::Arc` is reported once for each holder, unless it is a retainer, which is one
//! node.
//!
//! The size of a collection is that of its elements' slots: `capacity` slots for `Vec`,
//! `VecDeque`, `HashMap`, `HashSet` and `Dict`, `len` for `BTreeMap`, `BTreeSet` and boxed
//! slices. It leaves out the allocator's and the collection's own bookkeeping, such as a hash
//! table's control bytes and a B-tree's nodes, so it is an estimate, and a low one.

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

/// The tag that tells retainers of type `R` from those of other types at the same address: a
/// hash of its `TypeId`, which is the same wherever `R` is reported from. Two types share a tag
/// only by a hash collision, which would make the retainer reported second an edge to the node of
/// the first.
#[expect(
    clippy::cast_possible_truncation,
    reason = "a tag needs no more bits than a pointer has"
)]
fn type_tag<R: ?Sized + 'static>() -> usize {
    let mut hasher = DefaultHasher::new();
    TypeId::of::<R>().hash(&mut hasher);
    hasher.finish() as usize
}

/// Pops the node that a push made when it is dropped, so that the tracker's stack of nodes stays
/// balanced however the reporting in between ends.
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

    /// Reports `retainer` as a node of its own, the first time; later reports of the same
    /// retainer, which its address and type identify, only add an edge to that node, as for a C++
    /// retainer. A zero-sized retainer has no address of its own, so each report is a new node.
    ///
    /// Report the fields of the object being tracked, not values computed from them: two
    /// temporaries of the same type in the same stack slot would be taken for one retainer.
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
        let type_tag = type_tag::<R>();
        // SAFETY: the names are literals or null.
        let pushed = unsafe {
            ffi::memory_tracker_push_retainer(
                self.inner.as_mut(),
                address,
                type_tag,
                retainer.memory_name().as_ptr(),
                retainer.memory_self_size(),
                name_ptr(edge_name),
            )
        };
        if pushed {
            let mut node = PushedNode { tracker: self };
            retainer.memory_info(&mut node);
        }
    }

    /// Adds a node of `size` bytes named `name` for a container, with an edge to it from the
    /// object being tracked, and has `elements` report the container's elements to it, with
    /// `None` as their edge names.
    pub fn track_container(
        &mut self,
        edge_name: Option<&'static CStr>,
        name: &'static CStr,
        size: usize,
        elements: impl FnOnce(&mut Self),
    ) {
        // SAFETY: the names are literals or null.
        unsafe {
            ffi::memory_tracker_push_container(
                self.inner.as_mut(),
                name.as_ptr(),
                size,
                name_ptr(edge_name),
            );
        }
        let mut node = PushedNode { tracker: self };
        elements(&mut node);
    }

    /// Reports a Rust resource's Wrappable, as C++ reports a `jsg::Ref` field.
    pub(crate) fn track_wrappable(
        &mut self,
        edge_name: Option<&'static CStr>,
        wrappable: &ffi::Wrappable,
    ) {
        // SAFETY: the name is a literal or null.
        unsafe {
            ffi::memory_tracker_track_wrappable(
                self.inner.as_mut(),
                name_ptr(edge_name),
                wrappable,
            );
        }
    }

    /// Whether a persistent handle of the isolate of `state` can be reported: it belongs to the
    /// snapshot's isolate, which is not being disposed. Debug builds assert that it belongs to
    /// the snapshot's isolate, since a handle belongs in objects of its own isolate.
    pub(crate) fn can_track_handle_of(&self, state: &IsolateState) -> bool {
        let own = state.belongs_to(self.isolate);
        debug_assert!(
            own,
            "a heap snapshot found a Global or TracedGlobal of another isolate"
        );
        own && !state.is_closed()
    }

    /// Adds an edge to the value of `global`, unless it is empty.
    ///
    /// # Safety
    ///
    /// `global` is a handle of the tracker's isolate, or empty; see
    /// [`can_track_handle_of`](Self::can_track_handle_of).
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

/// How a value is reported to a heap snapshot when it is a field of a tracked object.
///
/// It is the counterpart of the overloads of C++ `MemoryTracker::trackField`.
/// [`MemoryTracker::track_field`] takes any type that implements it. A type that is a node of its
/// own implements [`MemoryRetainer`] and gets this impl from
/// [`jsg_memory_retainer!`](crate::jsg_memory_retainer).
///
/// # Rules for reporting code
///
/// The code that reports a value, [`VisitForMemoryInfo::visit_for_memory_info`],
/// [`MemoryRetainer::memory_info`] and every impl of this trait:
///
/// - must not panic: the tracker is called from C++, and a panic there aborts the process, as an
///   exception thrown while C++ builds the graph does;
/// - must not write into anything it can reach, a `Cell` above all: the impl for `Cell<T>`
///   reports the `T` in place, through `Cell::as_ptr`, as `Traced` traces it, and a `Cell::set`
///   or `Cell::replace` while it does would drop the `T` under it. Nothing checks this rule;
/// - must not call into JavaScript, and should allocate nothing.
///
/// A type that is part of a cycle through `std::rc::Rc` or `std::sync::Arc` must be a
/// [`MemoryRetainer`]: a retainer reached again is only an edge to its node, which ends the
/// cycle, while any other value is reported again each time it is reached, without end.
///
/// # Built-in implementations
///
/// - **Primitives** (`bool`, `char`, integers, floats, `()`, `SystemTime`, `Duration`, `&str`)
///   report nothing: they are part of the size of the object that holds them.
/// - **`String`** and **`Box<str>`** are a node of the memory they allocate, and
///   **`v8::BackingStore`** one of its byte length.
/// - **`Option<T>`**, **`Nullable<T>`**, **`NullOrUndefined<T>`**, **`Lenient<T>`**,
///   **`NonCoercible<T>`**, **`Cell<T>`**, **`RefCell<T>`**, **`std::rc::Rc<T>`**,
///   **`std::sync::Arc<T>`** and **`[T; N]`** report what they hold, if anything. A mutably
///   borrowed `RefCell` reports nothing. `Cell<T>` reports the `T` in place; see the rules for
///   reporting code above.
/// - **`Box<T>`** reports a retainer as its node, plain data as a node of its size, and anything
///   else as a node of the bytes that what it holds leaves uncounted, which what it holds
///   reports to.
/// - **Collections** (`Vec<T>`, `Box<[T]>`, `VecDeque<T>`, `Dict<V>`, `HashMap<K, V>`,
///   `BTreeMap<K, V>`, `HashSet<T>`, `BTreeSet<T>`) are a node of the memory they allocate, less
///   the bytes their elements' nodes count, which their elements report to; maps report their
///   entries under the edges `key` and `value`. A collection of plain data (see
///   [`Self::PLAIN_DATA`]) is one node, as C++ reports a `kj::Array` of numbers. An empty
///   collection reports nothing. A size counts the elements' slots, and leaves out bookkeeping
///   such as a hash table's control bytes or a B-tree's nodes, so it is a low estimate.
/// - **`jsg::Rc<R>`** reports the resource's node, as C++ reports a `jsg::Ref`.
/// - **`jsg::Weak<R>`** reports nothing.
/// - **`v8::Global<T>`** and **`v8::TracedGlobal<T>`** are an edge to the value in the V8 heap,
///   as C++ reports a `jsg::V8Ref`.
pub trait MemoryInfo {
    /// Whether a value of this type retains no memory beyond its own bytes, so that a
    /// collection of such values is reported as one node of the collection's size.
    const PLAIN_DATA: bool = false;

    /// How many of the bytes of `self` itself, out of its `size_of_val`, the nodes that `self`
    /// reports count, so that the collection or `Box` that holds `self` leaves them to those
    /// nodes. A [`MemoryRetainer`], whose node counts its own bytes, returns them; a wrapper that
    /// holds its value inline, such as `Option` or `Cell`, returns what the value returns; a
    /// value that holds what it reports out of line, such as a `Box` or `std::rc::Rc`, returns
    /// 0, the default. It follows the [rules for reporting code](Self#rules-for-reporting-code).
    fn self_bytes_reported(&self) -> usize {
        0
    }

    /// Reports `self` to `tracker`, under the edge `edge_name` from the object being tracked;
    /// `None` is the empty name of a collection's element. It follows the
    /// [rules for reporting code](Self#rules-for-reporting-code).
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>);
}

/// A type that is a node of its own in a heap snapshot.
///
/// It is the Rust counterpart of a C++ type with `JSG_MEMORY_INFO`. A retainer is reported with
/// [`MemoryTracker::track_retainer`]; [`jsg_memory_retainer!`](crate::jsg_memory_retainer)
/// implements [`MemoryInfo`] that way, so that [`MemoryTracker::track_field`] takes the type.
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

    /// Reports the fields that retain memory, with [`MemoryTracker::track_field`]. It follows the
    /// [rules for reporting code](MemoryInfo#rules-for-reporting-code): it must not panic, must
    /// not write into anything it can reach, a `Cell` above all, and must not call into
    /// JavaScript.
    fn memory_info(&self, tracker: &mut MemoryTracker<'_>);
}

/// Implements [`MemoryInfo`] for [`MemoryRetainer`] types, so that each value is a node of its
/// own: `jsg::jsg_memory_retainer!(Cache, Entry);`.
///
/// A retainer is `'static`, since its type tells it from a retainer of another type at the same
/// address.
///
/// A generic type writes the same impl itself: `track_as_field` calls
/// [`MemoryTracker::track_retainer`], and
/// [`self_bytes_reported`](MemoryInfo::self_bytes_reported) returns the bytes of `self` that its
/// node counts, the lesser of `size_of_val(self)` and `memory_self_size()`.
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

/// The fields that a resource reports to heap snapshots.
///
/// It is the Rust counterpart of C++ `visitForMemoryInfo`. `#[jsg_resource(memory_info)]` on the
/// resource's struct makes its node call this; without it a resource reports no fields.
///
/// ```ignore
/// #[jsg_resource(memory_info)]
/// struct Foo {
///     name: String,
///     child: Option<jsg::Rc<Bar>>,
/// }
///
/// impl jsg::VisitForMemoryInfo for Foo {
///     fn visit_for_memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
///         tracker.track_field(c"name", &self.name);
///         tracker.track_field(c"child", &self.child);
///     }
/// }
/// ```
pub trait VisitForMemoryInfo {
    /// Reports the fields that retain memory, with [`MemoryTracker::track_field`]. It follows the
    /// [rules for reporting code](MemoryInfo#rules-for-reporting-code): it must not panic, must
    /// not write into anything it can reach, a `Cell` above all, and must not call into
    /// JavaScript.
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

impl<T: MemoryInfo> MemoryInfo for std::cell::Cell<T> {
    const PLAIN_DATA: bool = T::PLAIN_DATA;

    fn self_bytes_reported(&self) -> usize {
        // SAFETY: as in `track_as_field`.
        unsafe { (*self.as_ptr()).self_bytes_reported() }
    }

    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        // SAFETY: a heap snapshot runs on the thread holding the isolate lock, and reporting code
        // writes into nothing it can reach and runs no JavaScript (see the rules for reporting
        // code on `MemoryInfo`), so nothing writes into the cell while the reference lives.
        // Nothing enforces that rule, as nothing does for `Traced` on `Cell`, which reads in
        // place in the same way. Reading in place gives a retainer the cell's own address.
        unsafe { (*self.as_ptr()).track_as_field(tracker, edge_name) };
    }
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

impl<T: MemoryInfo> MemoryInfo for std::rc::Rc<T> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        (**self).track_as_field(tracker, edge_name);
    }
}

impl<T: MemoryInfo> MemoryInfo for std::sync::Arc<T> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        (**self).track_as_field(tracker, edge_name);
    }
}

impl<T: MemoryInfo> MemoryInfo for Box<T> {
    fn track_as_field(&self, tracker: &mut MemoryTracker<'_>, edge_name: Option<&'static CStr>) {
        let inner: &T = self;
        if T::PLAIN_DATA {
            tracker.track_field_with_size(edge_name, size_of::<T>(), Some(c"Box"));
            return;
        }
        let uncounted = size_of::<T>().saturating_sub(inner.self_bytes_reported());
        if uncounted == 0 {
            // A retainer's node counts the whole allocation.
            inner.track_as_field(tracker, edge_name);
        } else {
            tracker.track_container(edge_name, c"Box", uncounted, |tracker| {
                inner.track_as_field(tracker, None);
            });
        }
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
