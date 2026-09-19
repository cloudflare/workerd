//! Wholly-safe wrappers for `kj::HttpHeaders` / `kj::HttpHeaderId`: `HeadersRef`, `Headers`, and
//! `CustomHeaderId` (plus the `HeaderId` / `HeaderTable` / `HeaderEntry` / `CustomHeader` aliases).
//!
//! This file inherits the crate-root `#![deny(unsafe_code)]` — it carries no `#![allow(unsafe_code)]`
//! and contains ZERO `unsafe`. Every FFI touch goes through a safe shim: the borrow-returning
//! getters are called via the safe wrapper fns in the `http` island (`crate::http::get_header*`),
//! and the remaining bridge shims (`new_http_headers`, `set_header`, `add_header`, `clear_headers`,
//! `clone_shallow`, `get_all_headers`) are already safe `fn`s. Re-exported from `crate::http` so the
//! `kj::http::{HeadersRef, Headers, CustomHeaderId, ...}` public paths are unchanged.
//!
//! `CustomHeaderId::from_ptr_slice` (the one `unsafe fn` associated with `CustomHeaderId`) lives in
//! the `http` island, not here.

use std::marker::PhantomData;

use kj_rs::KjOwn;

use crate::Result;
use crate::http::ffi;

pub type HeaderId = ffi::BuiltinIndicesEnum;
pub type HeaderTable = ffi::HttpHeaderTable;
pub type CustomHeader = ffi::HttpHeaderId;
pub type HeaderEntry = ffi::HttpHeaderEntry;

// TODO(tewaro) soon: replace by enum HeaderId

/// Non-owning reference to a `kj::HttpHeaderId`.
///
/// `CustomHeader` is an opaque CXX type representing `HttpHeaderId` and can only be passed by
/// reference across the FFI boundary. This wrapper makes the borrow lifetime explicit and provides
/// a safe Rust handle.
///
/// `repr(transparent)` guarantees the same layout as `&ffi::HttpHeaderId` (i.e. a single
/// pointer), which allows safe reinterpretation of `&[*const HttpHeaderId]` slices received
/// from C++ into `&[CustomHeaderId]` via [`CustomHeaderId::from_ptr_slice`] (defined in the
/// `http` island).
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct CustomHeaderId<'a>(pub(crate) &'a CustomHeader);

impl<'a> From<&'a CustomHeader> for CustomHeaderId<'a> {
    fn from(value: &'a CustomHeader) -> Self {
        CustomHeaderId(value)
    }
}

/// Non-owning constant reference to `kj::HttpHeaders`
#[derive(Clone, Copy)]
pub struct HeadersRef<'a>(&'a ffi::HttpHeaders);

impl HeadersRef<'_> {
    pub fn get(&self, id: HeaderId) -> Option<&[u8]> {
        crate::http::get_header(self.0, id)
    }

    /// Return every set header as a (name, value) pair, in kj-http serialization order (indexed
    /// headers in table id order first, then unindexed headers in insertion order). Duplicate
    /// names produce one entry per value.
    #[must_use]
    pub fn entries(&self) -> Vec<HeaderEntry> {
        ffi::get_all_headers(self.0)
    }

    /// Look up a header by its `kj::HttpHeaderId`. This works for both builtin headers and custom
    /// headers registered via `HttpHeaderTable::Builder::add()`.
    pub fn get_by_id(&self, id: CustomHeaderId<'_>) -> Option<&[u8]> {
        crate::http::get_header_by_id(self.0, id.0)
    }

    #[must_use]
    pub fn clone_shallow(&self) -> Headers<'_> {
        Headers {
            own: ffi::clone_shallow(self.0),
            _marker: PhantomData,
        }
    }
}

impl<'a> HeadersRef<'a> {
    /// Look up a header value by name, matching case-insensitively.
    ///
    /// If the same header name appears more than once, the first set value is returned (matching
    /// [`get`](Self::get) / [`get_by_id`](Self::get_by_id)). Only headers that have been explicitly
    /// set are considered; a header registered in the header table but never set returns `None`.
    pub fn get_by_name(&self, name: &str) -> Option<&'a [u8]> {
        crate::http::get_header_by_name(self.0, name)
    }

    /// The underlying `kj::HttpHeaders`, for passing to FFI functions that take a `const&`.
    #[must_use]
    pub fn as_ffi(self) -> &'a ffi::HttpHeaders {
        self.0
    }
}

impl<'a> From<&'a ffi::HttpHeaders> for HeadersRef<'a> {
    fn from(value: &'a ffi::HttpHeaders) -> Self {
        HeadersRef(value)
    }
}

/// `HttpHeaders` that `kj::Own` the underlying C++ header object.
///
/// Notice, that despite the fact that headers are fully owned, because of a `shallowClone`
/// method, data might not be owned: hence the lifetime parameter.
pub struct Headers<'a> {
    own: KjOwn<ffi::HttpHeaders>,
    _marker: PhantomData<&'a ffi::HttpHeaders>,
}

impl<'a> Headers<'a> {
    #[must_use]
    pub fn new(table: &'a HeaderTable) -> Self {
        Self {
            own: ffi::new_http_headers(table),
            _marker: PhantomData,
        }
    }

    pub fn set(&mut self, id: HeaderId, value: &str) {
        ffi::set_header(self.own.as_mut(), id, value);
    }

    /// Append a header by name, using indexed storage if `name` is registered in the header
    /// table. `value` may contain arbitrary bytes except `\0`, `\r` and `\n` (rejected by
    /// kj-http with an error).
    pub fn add(&mut self, name: &str, value: &[u8]) -> Result<()> {
        Ok(ffi::add_header(self.own.as_mut(), name, value)?)
    }

    /// Append many headers in one shot, borrowing name/value spans into a single kj-owned arena
    /// buffer instead of allocating two `kj::String`s per header (the [`add`](Self::add) cost).
    ///
    /// Behaviourally identical to calling [`add`](Self::add) for each `(name, value)` in order:
    /// same indexing, same duplicate-concatenation, same validation. The first header kj-http
    /// rejects (`\0`/`\r`/`\n` in the value, or an invalid name) returns `Err`, with the headers
    /// added before it left in place — exactly as the per-call loop behaved (the caller discards
    /// the whole `Headers` on error). The borrowed spans stay valid because the arena is attached
    /// to the underlying `kj::HttpHeaders` via `takeOwnership`; see `add_headers_arena` in ffi.h.
    pub fn add_all<'n, 'v>(
        &mut self,
        entries: impl IntoIterator<Item = (&'n str, &'v [u8])>,
    ) -> Result<()> {
        let mut arena: Vec<u8> = Vec::new();
        // Two u32 lengths per header (name, value); see add_headers_arena's packing contract.
        let mut lens: Vec<u32> = Vec::new();
        for (name, value) in entries {
            arena.extend_from_slice(name.as_bytes());
            arena.push(0);
            arena.extend_from_slice(value);
            arena.push(0);
            lens.push(name.len() as u32);
            lens.push(value.len() as u32);
        }
        Ok(ffi::add_headers_arena(self.own.as_mut(), &arena, &lens)?)
    }

    /// Remove all headers, leaving the collection empty. This is a destructive operation; after
    /// calling it, [`get`](HeadersRef::get) for any previously-set header returns `None`.
    pub fn clear(&mut self) {
        ffi::clear_headers(self.own.as_mut());
    }

    pub fn as_ref(&'a self) -> HeadersRef<'a> {
        HeadersRef(self.own.as_ref())
    }
}

impl<'a, 'b> From<&'b Headers<'a>> for HeadersRef<'b> {
    fn from(value: &'b Headers<'a>) -> Self {
        value.as_ref()
    }
}
