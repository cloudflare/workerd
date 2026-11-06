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
//!   `#[jsg_resource(memory_info)]` on the struct opts the resource in and generates it from the
//!   named fields; `#[jsg_resource(custom_memory_info)]` opts in with a hand-written impl.
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
//! retainers it holds inline, directly or in an `Option`, `jsg::Cell` or other inline wrapper, to their
//! own nodes (see [`MemoryInfo::self_bytes_reported`]). An allocation that several
//! `std::rc::Rc`s or `std::sync::Arc`s share is one node, as a retainer is. Nodes nest at most
//! [`MemoryTracker::MAX_DEPTH`] deep.
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
    /// How deep reporting nests nodes. A retainer, container, shared allocation or resource
    /// that would be this deep is a leaf: a node of its own size, with nothing inside it, which
    /// is not deduplicated, so that a shallower path to the same value still reports it in full.
    /// A resource with a JavaScript wrapper is an edge to the wrapper instead. It bounds the stack
    /// that reporting a long chain uses, such as a linked list of retainers or of resources.
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

    /// Reports `retainer` as a node of its own, the first time; later reports of the same
    /// retainer, which its address and type identify, only add an edge to that node, as for a C++
    /// retainer. A zero-sized retainer has no address of its own, so each report is a new node.
    ///
    /// Report the fields of the object being tracked, not values computed from them: two
    /// temporaries of the same type in the same stack slot would be taken for one retainer.
    ///
    /// A retainer that would be [`Self::MAX_DEPTH`] nodes deep is a leaf.
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

    /// Adds a node of `size` bytes named `name`, identified by `key`, an address and a type tag
    /// (an address of 0 identifies nothing), with an edge to it from the object being tracked,
    /// and has `contents` report what it holds to it; only the edge if the node was added before.
    /// A node that would be [`Self::MAX_DEPTH`] deep is a leaf.
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

    /// Adds a node of `size` bytes named `name`, with nothing inside it, with an edge to it from
    /// the object being tracked. Unlike [`Self::track_field_with_size`], it adds one of size 0.
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

    /// Adds a node of `size` bytes named `name` for a container, with an edge to it from the
    /// object being tracked, and has `elements` report the container's elements to it, with
    /// `None` as their edge names. A container that would be [`Self::MAX_DEPTH`] nodes deep is
    /// a leaf.
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

    /// Reports a Rust resource's Wrappable, a resource of the snapshot's isolate, as C++ reports
    /// a `jsg::Ref` field. One that would be [`Self::MAX_DEPTH`] deep is an edge to its wrapper,
    /// or a leaf if it has none.
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

    /// Whether a handle or resource of the isolate of `state` can be reported: it belongs to the
    /// snapshot's isolate, which is not being disposed. One of another isolate, which Rust code
    /// can store in a resource, is left out.
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
/// - must not write into anything it can reach: a snapshot reports what it finds as it is. A
///   [`crate::Cell`] reports its value in place and panics on a write meanwhile, and a `RefCell`
///   is borrowed while it reports its value, so a `borrow_mut` panics; either aborts the process;
/// - must not call into JavaScript, and should allocate nothing.
///
/// # Built-in implementations
///
/// - **Primitives** (`bool`, `char`, integers, floats, `()`, `SystemTime`, `Duration`, `&str`)
///   report nothing: they are part of the size of the object that holds them.
/// - **`String`** and **`Box<str>`** are a node of the memory they allocate, and
///   **`v8::BackingStore`** one of its byte length.
/// - **`Option<T>`**, **`Nullable<T>`**, **`NullOrUndefined<T>`**, **`Lenient<T>`**,
///   **`NonCoercible<T>`**, **`jsg::Cell<T>`**, **`RefCell<T>`**, **`jsg::RefCell<T>`**,
///   **`jsg::Arguments<T>`** and **`[T; N]`** report what they hold, if anything. A mutably
///   borrowed `RefCell` reports nothing. A `jsg::Cell<T>` reports the `T` in place; see the rules
///   for reporting code above. A **`std::cell::Cell<T>`** exists only for `T: Copy`, and is plain
///   data, which reports nothing; hold a retainer in a `jsg::Cell`.
/// - **`Box<T>`**, **`std::rc::Rc<T>`** and **`std::sync::Arc<T>`** report a retainer as its node,
///   plain data as a node of its size, and anything else as a node of the bytes that what they
///   hold leaves uncounted, which what they hold reports to. The node of an allocation that
///   several `Rc`s or `Arc`s share is one node, which the first report adds and later ones only
///   have an edge to, so that the work is linear in the number of pointers and a cycle of them
///   ends.
/// - **Collections** (`Vec<T>`, `Box<[T]>`, `VecDeque<T>`, `Dict<V>`, `HashMap<K, V>`,
///   `BTreeMap<K, V>`, `HashSet<T>`, `BTreeSet<T>`) are a node of the memory they allocate, less
///   the bytes their elements' nodes count, which their elements report to; maps report their
///   entries under the edges `key` and `value`. A collection of plain data (see
///   [`Self::PLAIN_DATA`]) is one node, as C++ reports a `kj::Array` of numbers. An empty
///   collection reports nothing. A size counts the elements' slots, and leaves out bookkeeping
///   such as a hash table's control bytes or a B-tree's nodes, so it is a low estimate.
/// - **`jsg::Rc<R>`** reports the resource's node, as C++ reports a `jsg::Ref`; one of another
///   isolate reports nothing.
/// - **`jsg::Weak<R>`** reports nothing.
/// - **`v8::Global<T>`** and **`v8::TracedGlobal<T>`** are an edge to the value in the V8 heap,
///   as C++ reports a `jsg::V8Ref`, and so are **`jsg::TracedPromise<T>`**,
///   **`jsg::TracedPromiseResolver<T>`**, **`jsg::AsyncContextFrame`** and
///   **`jsg::TracedAsyncContextFrame`**, of which the root frame reports nothing. A handle of
///   another isolate reports nothing.
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
/// resource's struct generates it, reporting each named field under its name, as the generated
/// `Traced` traces each field, so that a field added later is reported too, or fails to compile
/// if its type has no [`MemoryInfo`]. `#[jsg_memory_info(skip)]` on a field leaves it out:
///
/// ```ignore
/// #[jsg_resource(memory_info)]
/// struct Foo {
///     name: String,
///     child: Option<jsg::Rc<Bar>>,
///     #[jsg_memory_info(skip)]
///     handle: SomeHandleWithoutMemoryInfo,
/// }
/// ```
///
/// `#[jsg_resource(custom_memory_info)]` instead leaves the impl to the author, as for C++, to
/// report under other names or report values that are not fields:
///
/// ```ignore
/// #[jsg_resource(custom_memory_info)]
/// struct Foo {
///     name: String,
/// }
///
/// impl jsg::VisitForMemoryInfo for Foo {
///     fn visit_for_memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
///         tracker.track_field(c"name", &self.name);
///     }
/// }
/// ```
///
/// Without either flag a resource reports no fields.
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

/// Plain data, only for `T: Copy`, which owns nothing outside its bytes. Reporting the `T` in
/// place would let reporting code that wrote into the cell drop the `T` under it, and reporting
/// a copy would report a retainer at the copy's address, so a retainer belongs in a
/// [`crate::Cell`], which reports any `T` in place.
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

/// Reports `inner`, which a `Box` named `name` holds in an allocation of its own: plain data as a
/// node of its size, a retainer as its node, which counts the whole value, and anything else as a
/// node of the bytes that `inner` leaves uncounted, to which `inner` reports.
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

/// Reports `inner`, which an `std::rc::Rc` or `std::sync::Arc` named `name`, whose kind `K`
/// tells its allocations from other values at the same address, shares from the allocation at
/// `address`, as a `Box` reports what it holds, except that a node for the
/// allocation is one node however many pointers share it, as a retainer is: the first report adds
/// it, and later ones only an edge to it. That keeps the work linear in the number of pointers,
/// and ends a cycle of pointers. A retainer is its own node, which deduplicates itself. A
/// reference count is bookkeeping, which sizes leave out.
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
