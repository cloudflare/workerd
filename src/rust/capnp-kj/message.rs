// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Messages crossing between Rust and C++.
//!
//! [`RustMessage`] is a message Rust built, which C++ copies from. [`CxxMessageReader`] reads a
//! message C++ built, in place.

use capnp::any_pointer;
use capnp::message;
use capnp::private::capability::ClientHook;
use capnp::traits::Imbue;
use capnp::traits::ImbueMut;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;

use crate::client::KjClient;
use crate::ffi::bridge;

/// capnp-rust's capability table: the capabilities a message refers to, by index.
pub type CapTable = Vec<Option<Box<dyn ClientHook>>>;

/// The size of a message's first segment, in words: capnp-rust's and KJ's default. A smaller one
/// makes messages of a few KiB need a second segment.
const FIRST_SEGMENT_WORDS: u32 = 1024;

/// How many free first segments a thread keeps. A call needs one for its params or its results,
/// so this covers that many calls being built at once on a thread before segments come from the
/// heap again.
const MAX_POOLED_SEGMENTS: usize = 16;

/// Zeroed first segments, `FIRST_SEGMENT_WORDS` long, ready for the next message. Frees them when
/// its thread exits.
struct SegmentPool(Vec<std::ptr::NonNull<u8>>);

impl Drop for SegmentPool {
    fn drop(&mut self) {
        for segment in self.0.drain(..) {
            // Safety: every pooled segment came from `alloc_zeroed` with this layout.
            #[expect(unsafe_code, reason = "frees a pooled segment")]
            unsafe {
                std::alloc::dealloc(segment.as_ptr(), first_segment_layout());
            }
        }
    }
}

thread_local! {
    static SEGMENT_POOL: std::cell::RefCell<SegmentPool> =
        const { std::cell::RefCell::new(SegmentPool(Vec::new())) };
}

fn first_segment_layout() -> std::alloc::Layout {
    match std::alloc::Layout::from_size_align(FIRST_SEGMENT_WORDS as usize * 8, 8) {
        Ok(layout) => layout,
        Err(_) => unreachable!(),
    }
}

/// capnp-rust's `HeapAllocator`, except that a message's first segment comes from, and returns
/// to, a per-thread pool. capnp-rust needs its segments zeroed; a pooled segment is re-zeroed when
/// it returns, but only for the words the message used, rather than allocating and zeroing all of
/// it for every message.
pub struct PooledAllocator {
    first: Option<std::ptr::NonNull<u8>>,
    heap: message::HeapAllocator,
}

impl PooledAllocator {
    fn new() -> Self {
        Self {
            first: None,
            heap: message::HeapAllocator::new(),
        }
    }
}

#[expect(
    unsafe_code,
    reason = "implements capnp-rust's Allocator, an unsafe trait"
)]
// Safety: every segment is zeroed when it is handed out: a fresh one by `alloc_zeroed`, and a
// pooled one because the message that last used it re-zeroed all the words it could have written.
unsafe impl message::Allocator for PooledAllocator {
    fn allocate_segment(&mut self, minimum_size: u32) -> (std::ptr::NonNull<u8>, u32) {
        if self.first.is_some() || minimum_size > FIRST_SEGMENT_WORDS {
            return self.heap.allocate_segment(minimum_size);
        }
        // While its thread exits, after the pool is gone, segments come from and go to the heap.
        let segment = SEGMENT_POOL
            .try_with(|pool| pool.borrow_mut().0.pop())
            .ok()
            .flatten()
            .unwrap_or_else(|| {
                let layout = first_segment_layout();
                // Safety: the layout is not zero-sized.
                #[expect(unsafe_code, reason = "allocates a segment")]
                let ptr = unsafe { std::alloc::alloc_zeroed(layout) };
                std::ptr::NonNull::new(ptr)
                    .unwrap_or_else(|| std::alloc::handle_alloc_error(layout))
            });
        self.first = Some(segment);
        (segment, FIRST_SEGMENT_WORDS)
    }

    #[expect(unsafe_code, reason = "frees or re-zeroes a segment")]
    unsafe fn deallocate_segment(
        &mut self,
        ptr: std::ptr::NonNull<u8>,
        word_size: u32,
        words_used: u32,
    ) {
        if self.first != Some(ptr) {
            // Safety: the caller passes a segment `allocate_segment` returned, and this one came
            // from `heap`.
            unsafe { self.heap.deallocate_segment(ptr, word_size, words_used) };
            return;
        }
        self.first = None;
        // Safety: `ptr` is this allocator's first segment, `FIRST_SEGMENT_WORDS` long, and
        // `words_used <= word_size`. Only the first `words_used` words can have been written.
        unsafe { std::ptr::write_bytes(ptr.as_ptr(), 0, words_used as usize * 8) };
        let returned = SEGMENT_POOL
            .try_with(|pool| {
                let mut pool = pool.borrow_mut();
                if pool.0.len() < MAX_POOLED_SEGMENTS {
                    pool.0.push(ptr);
                    true
                } else {
                    false
                }
            })
            .unwrap_or(false);
        if !returned {
            // Safety: the segment came from `alloc_zeroed` with this layout.
            unsafe { std::alloc::dealloc(ptr.as_ptr(), first_segment_layout()) };
        }
    }
}

/// A message built in Rust, with its capability table.
pub struct RustMessage {
    builder: message::Builder<PooledAllocator>,
    caps: CapTable,
}

impl RustMessage {
    pub fn new() -> Self {
        let mut builder = message::Builder::new(PooledAllocator::new());
        // Allocates the root pointer, so that the message has a segment for C++ to read even
        // when nothing is written to it.
        builder.init_root::<any_pointer::Builder<'_>>();
        Self {
            builder,
            caps: Vec::new(),
        }
    }

    /// The root of the message, imbued with its capability table.
    pub fn root(&mut self) -> capnp::Result<any_pointer::Builder<'_>> {
        let mut root: any_pointer::Builder<'_> = self.builder.get_root()?;
        root.imbue_mut(&mut self.caps);
        Ok(root)
    }

    pub fn segment_count(&self) -> usize {
        self.builder.get_segments_for_output().len()
    }

    pub fn segment(&self, index: usize) -> &[u8] {
        self.builder
            .get_segments_for_output()
            .get(index)
            .copied()
            .unwrap_or(&[])
    }

    pub fn cap_count(&self) -> usize {
        self.caps.len()
    }

    /// Takes capability `index` out of the table, as a C++ capability. Every capability in a
    /// Rust message came from C++ (see [`KjClient`]), so this unwraps it rather than wrapping it.
    pub fn take_cap(&mut self, index: usize) -> KjMaybe<KjOwn<bridge::ClientHook>> {
        let Some(slot) = self.caps.get_mut(index) else {
            return KjMaybe::None;
        };
        match slot.take() {
            Some(hook) => KjMaybe::Some(KjClient::into_kj(hook)),
            None => KjMaybe::None,
        }
    }
}

impl Default for RustMessage {
    fn default() -> Self {
        Self::new()
    }
}

/// The segments of a C++ message, read in place.
///
/// capnp-rust asks for a segment on every pointer it follows, so the segments are looked up once,
/// when the message arrives, rather than over FFI each time.
pub struct CxxSegments {
    /// The first segment, which is usually the only one; kept inline so that a one-segment
    /// message costs no allocation.
    first: *const [u8],
    rest: Vec<*const [u8]>,
    // Declared last, so that it is dropped after the views into it. Never read: it is held only
    // to keep the segments alive.
    #[expect(dead_code, reason = "owns the memory `first` and `rest` point into")]
    message: KjOwn<bridge::CxxMessage>,
}

impl CxxSegments {
    fn new(message: KjOwn<bridge::CxxMessage>) -> Self {
        let count = bridge::message_segment_count(&message);
        // The root pointer is in the first segment, so a message always has one.
        debug_assert!(count > 0, "a C++ message has no segments");
        let segment = |index| std::ptr::from_ref(bridge::message_segment(&message, index));
        let first = segment(0);
        let rest = (1..count).map(segment).collect();
        Self {
            first,
            rest,
            message,
        }
    }
}

impl message::ReaderSegments for CxxSegments {
    fn get_segment(&self, index: u32) -> Option<&[u8]> {
        let segment = match usize::try_from(index).ok()? {
            0 => self.first,
            index => *self.rest.get(index - 1)?,
        };
        #[expect(
            unsafe_code,
            reason = "reads a segment of the C++ message that `self` owns"
        )]
        // Safety: the segments belong to `self.message`, which lives as long as `self`, on the
        // heap, and whose segments never change once it is built (taking its capabilities
        // changes only its capability table).
        Some(unsafe { &*segment })
    }

    fn len(&self) -> usize {
        1 + self.rest.len()
    }
}

/// A message built in C++, read in place, with its capabilities moved into a capnp-rust table.
pub struct CxxMessageReader {
    reader: message::Reader<CxxSegments>,
    caps: CapTable,
}

impl CxxMessageReader {
    pub fn new(mut message: KjOwn<bridge::CxxMessage>) -> Self {
        let cap_count = bridge::message_cap_count(&message);
        let caps = (0..cap_count)
            .map(|index| {
                Option::<KjOwn<bridge::ClientHook>>::from(bridge::message_take_cap(
                    message.pin_mut(),
                    index,
                ))
                .map(|hook| Box::new(KjClient::new(hook)) as Box<dyn ClientHook>)
            })
            .collect();
        let mut options = message::ReaderOptions::new();
        // The message was already a valid message on the C++ side, whose own limits applied.
        options.traversal_limit_in_words(None);
        Self {
            reader: message::Reader::new(CxxSegments::new(message), options),
            caps,
        }
    }

    pub fn root(&self) -> capnp::Result<any_pointer::Reader<'_>> {
        let mut root: any_pointer::Reader<'_> = self.reader.get_root()?;
        root.imbue(&self.caps);
        Ok(root)
    }
}
