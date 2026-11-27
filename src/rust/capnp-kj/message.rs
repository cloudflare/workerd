// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Messages crossing between Rust and C++.
//!
//! [`RustMessage`] is a message Rust built, which C++ copies from. [`CxxMessageReader`] reads a
//! message C++ copied, in place.

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

/// A message built in Rust, with its capability table.
pub struct RustMessage {
    builder: message::Builder<message::HeapAllocator>,
    caps: CapTable,
}

impl RustMessage {
    pub fn new() -> Self {
        let mut builder = message::Builder::new_default();
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

    /// Returns a new reference to capability `index`, as a C++ capability. Every capability in a
    /// Rust message came from C++ (see [`KjClient`]), so this unwraps it rather than wrapping it.
    pub fn get_cap(&self, index: usize) -> KjMaybe<KjOwn<bridge::ClientHook>> {
        match self.caps.get(index) {
            Some(Some(hook)) => KjMaybe::Some(KjClient::into_kj(hook.add_ref())),
            _ => KjMaybe::None,
        }
    }
}

impl Default for RustMessage {
    fn default() -> Self {
        Self::new()
    }
}

/// The segments of a C++ message, read in place.
pub struct CxxSegments {
    message: KjOwn<bridge::CxxMessage>,
}

impl message::ReaderSegments for CxxSegments {
    fn get_segment(&self, index: u32) -> Option<&[u8]> {
        let index = usize::try_from(index).ok()?;
        if index < bridge::message_segment_count(&self.message) {
            Some(bridge::message_segment(&self.message, index))
        } else {
            None
        }
    }

    fn len(&self) -> usize {
        bridge::message_segment_count(&self.message)
    }
}

/// A message built in C++, read in place, with new references to its capabilities in a capnp-rust
/// table.
pub struct CxxMessageReader {
    reader: message::Reader<CxxSegments>,
    caps: CapTable,
}

impl CxxMessageReader {
    pub fn new(mut message: KjOwn<bridge::CxxMessage>) -> Self {
        let cap_count = bridge::message_cap_count(&message);
        let caps = (0..cap_count)
            .map(|index| {
                Option::<KjOwn<bridge::ClientHook>>::from(bridge::message_get_cap(
                    message.pin_mut(),
                    index,
                ))
                .map(|hook| Box::new(KjClient::new(hook)) as Box<dyn ClientHook>)
            })
            .collect();
        let mut options = message::ReaderOptions::new();
        // The message needs no limits: it is a copy of a message C++ already read under its own
        // limits, or built in this process.
        options.traversal_limit_in_words(None);
        options.nesting_limit(i32::MAX);
        Self {
            reader: message::Reader::new(CxxSegments { message }, options),
            caps,
        }
    }

    pub fn root(&self) -> capnp::Result<any_pointer::Reader<'_>> {
        let mut root: any_pointer::Reader<'_> = self.reader.get_root()?;
        root.imbue(&self.caps);
        Ok(root)
    }
}
