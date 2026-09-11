//! FFI-island file: the `#[cxx::bridge] mod ffi` for `kj::HttpHeaders` / `HttpService` /
//! `HttpHeaderId`. This file is the crate's unsafe island for the HTTP bridge: its file-top
//! `#![allow(unsafe_code)]` re-allows unsafe against the crate-root `#![deny(unsafe_code)]`. It
//! contains ONLY (a) the `#[cxx::bridge] mod ffi`, (b) the safe wrapper fns over the borrow-
//! returning header getters (`get_header`/`get_header_by_id`/`get_header_by_name` — `unsafe` only
//! because cxx blanket-marks lifetime-annotated extern fns), and (c) `CustomHeaderId::from_ptr_slice`
//! (the pointer-slice reinterpretation). The wholly-safe wrapper TYPES live in `headers.rs`
//! (`HeadersRef`/`Headers`/`CustomHeaderId`) and `service.rs` (`Service`/`CxxService`/
//! `ServiceResponse`/`ConnectResponse`) and are re-exported below so the `kj::http::*` public
//! paths are unchanged. The bridge stays in this file (rather than a wholly-safe split) because
//! the `kj::http::ffi` module path and the generated `http.rs.h` header are referenced across the
//! workerd consumers.
#![allow(unsafe_code)]

// Re-export the wholly-safe wrappers so the `kj::http::{HeadersRef, Headers, CustomHeaderId,
// HeaderId, HeaderTable, HeaderEntry, CustomHeader}` and `kj::http::{Service, CxxService,
// ServiceResponse, ConnectResponse, Method, ConnectSettings}` public paths resolve exactly as
// before the island/wholly-safe split.
// The wholly-safe HTTP/1.1 text codec (serialization/parsing/validation, see codec.rs), pulled
// into this scope because cxx resolves the bridge's extern "Rust" items against the enclosing
// module.
pub use crate::codec::*;
pub use crate::headers::*;
pub use crate::service::*;

#[cxx::bridge(namespace = "kj::rust")]
// The named `'a` on the shared `HttpConnectSettings<'a>` struct cannot be elided to `'_` in a
// struct definition, and the explicit lifetime is required by this repo's cxx::bridge dialect.
#[allow(clippy::elidable_lifetime_names)]
pub mod ffi {
    unsafe extern "C++" {
        include!("kj-rs-http/ffi.h");
    }

    /// Corresponds to `kj::HttpMethod`.
    /// Values are automatically assigned by `cxx` because of extern declaration below.
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    #[repr(u32)]
    enum HttpMethod {
        GET,
        HEAD,
        POST,
        PUT,
        DELETE,
        PATCH,
        PURGE,
        OPTIONS,
        TRACE,
        COPY,
        LOCK,
        MKCOL,
        MOVE,
        PROPFIND,
        PROPPATCH,
        SEARCH,
        UNLOCK,
        ACL,
        REPORT,
        MKACTIVITY,
        CHECKOUT,
        MERGE,
        MSEARCH,
        NOTIFY,
        SUBSCRIBE,
        UNSUBSCRIBE,
        QUERY,
        BAN,
    }
    unsafe extern "C++" {
        type HttpMethod;
    }

    // --- HttpHeaderId
    // Opaque handle to a kj::HttpHeaderId, which identifies a header by numeric index in an
    // HttpHeaderTable. This supports both builtin headers and custom headers registered via
    // HttpHeaderTable::Builder::add(). Pass these by reference from C++ to Rust and back.

    unsafe extern "C++" {
        type HttpHeaderId;
    }

    // --- HttpHeaders
    // TODO(when needed): support HttpHeaderId creation from rust.

    /// Corresponds to `kj::HttpHeaders::BuiltinIndicesEnum`.
    /// Values are automatically assigned by `cxx` because of extern declaration below.
    #[derive(Debug, PartialEq, Eq, Copy, Clone)]
    #[repr(u32)]
    pub enum BuiltinIndicesEnum {
        CONNECTION,
        KEEP_ALIVE,
        TE,
        TRAILER,
        UPGRADE,
        CONTENT_LENGTH,
        TRANSFER_ENCODING,
        SEC_WEBSOCKET_KEY,
        SEC_WEBSOCKET_VERSION,
        SEC_WEBSOCKET_ACCEPT,
        SEC_WEBSOCKET_EXTENSIONS,
        HOST,
        DATE,
        LOCATION,
        CONTENT_TYPE,
        RANGE,
        CONTENT_RANGE,
    }

    // Each `unsafe fn` below carries a per-decl `# Safety` doc for humans; the block-level
    // allow matches kj-rs/lib.rs because the cxx::bridge expansion does not propagate those
    // docs onto the generated shims that clippy's `missing_safety_doc` inspects.
    #[allow(clippy::missing_safety_doc)]
    unsafe extern "C++" {
        type BuiltinIndicesEnum;
        type HttpHeaderTable;
        type HttpHeaders;
        fn new_http_headers(table: &HttpHeaderTable) -> KjOwn<HttpHeaders>;
        fn clone_shallow(this_: &HttpHeaders) -> KjOwn<HttpHeaders>;
        fn clear_headers(this_: Pin<&mut HttpHeaders>);
        fn set_header(this_: Pin<&mut HttpHeaders>, id: BuiltinIndicesEnum, value: &str);
        /// # Safety
        ///
        /// The returned slice borrows the header value bytes owned by `this_`; the `'a`
        /// lifetime ties it to the `&'a HttpHeaders` borrow so Rust prevents use-after-free.
        /// The caller must ensure the C++ `HttpHeaders` (and its header table) outlive the
        /// borrow and are not mutated while it is held — guaranteed here by the shared `&'a`
        /// borrow and kj-http's stable, immutable header storage.
        unsafe fn get_header<'a>(
            this_: &'a HttpHeaders,
            id: BuiltinIndicesEnum,
        ) -> KjMaybe<&'a [u8]>;
        /// # Safety
        ///
        /// Same contract as [`get_header`]: the returned slice borrows from `this_` for `'a`.
        /// The `HttpHeaderId` must belong to the same header table as `this_`.
        unsafe fn get_header_by_id<'a>(
            this_: &'a HttpHeaders,
            id: &HttpHeaderId,
        ) -> KjMaybe<&'a [u8]>;
        /// # Safety
        ///
        /// Same contract as [`get_header`]: the returned slice borrows from `this_` for `'a`.
        unsafe fn get_header_by_name<'a>(this_: &'a HttpHeaders, name: &str) -> KjMaybe<&'a [u8]>;

        /// Corresponds to `kj::HttpHeaders::forEach`: returns every set header as a (name, value)
        /// pair. Indexed (table-registered) headers come first, in table id order, followed by
        /// unindexed headers in insertion order — matching kj-http's own serialization order.
        fn get_all_headers(this_: &HttpHeaders) -> Vec<HttpHeaderEntry>;

        /// Corresponds to `kj::HttpHeaders::add(kj::String, kj::String)`: appends a header,
        /// automatically using indexed storage if `name` is registered in the header table.
        /// `value` may contain arbitrary non-UTF-8 bytes (obs-text), but kj-http rejects
        /// `\0`, `\r` and `\n` (throws, surfaced here as `Err`).
        fn add_header(this_: Pin<&mut HttpHeaders>, name: &str, value: &[u8]) -> Result<()>;

        /// Batch form of [`add_header`]: appends every header from a single packed `arena`
        /// buffer, borrowing name/value spans into a kj-owned copy of it (one allocation for the
        /// whole block) instead of two `kj::String`s per header. `arena` packs, per header in
        /// add order, `name` bytes, a `0`, `value` bytes, a `0`; `lens` is the flat
        /// `[name_len, value_len, ...]` sequence (two `u32` per header). Validation, ordering and
        /// error semantics are identical to repeated `add_header` calls (first invalid header
        /// returns `Err`). See `add_headers_arena` in ffi.h for the borrowing/lifetime contract.
        fn add_headers_arena(
            this_: Pin<&mut HttpHeaders>,
            arena: &[u8],
            lens: &[u32],
        ) -> Result<()>;
    }

    /// A single HTTP header (name, value) pair.
    ///
    /// The value is a byte vector because kj-http allows non-UTF-8 (obs-text) bytes in header
    /// values; the name is always ASCII.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct HttpHeaderEntry {
        pub name: String,
        pub value: Vec<u8>,
    }

    // --- The HTTP/1.1 text codec (codec.rs)
    //
    // Byte-exact ports of kj-http's HttpHeaders serialization, in-place header parsing, start-
    // line validation, and Range-header parsing. Under the rust I/O backend the C++ codec TU
    // (kj-http-impl) is not linked; workerd's kj-http shim (kj-http-tokio.c++) defines the
    // evicted kj:: symbols over these.

    /// One parsed header's name/value byte ranges within the parse buffer (each NUL-terminated
    /// in place, so the C++ side forms in-buffer `kj::StringPtr`s over them — kj's own
    /// parseHeaders() storage scheme).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct HttpHeaderSpan {
        pub name_begin: usize,
        pub name_end: usize,
        pub value_begin: usize,
        pub value_end: usize,
    }

    /// Result of [`parse_http_headers`]/[`parse_http_headers_trimmed`]: kj's
    /// `HttpHeaders::tryParse()` bool plus, on success, the parsed header spans.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct HttpHeaderParseResult {
        pub ok: bool,
        pub spans: Vec<HttpHeaderSpan>,
    }

    /// One satisfiable byte range (kj's `HttpByteRange`): inclusive start/end offsets.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct HttpRangeSpan {
        pub start: u64,
        pub end: u64,
    }

    /// Result of [`parse_range_header`], kj's `HttpRanges` shape: `kind` 0 = the `ranges`
    /// array, 1 = `HttpEverythingRange`, 2 = `HttpUnsatisfiableRange` (see the
    /// `HTTP_RANGES_KIND_*` constants in codec.rs).
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct HttpRangesResult {
        pub kind: u8,
        pub ranges: Vec<HttpRangeSpan>,
    }

    extern "Rust" {
        /// Incremental serializer for one HTTP/1.1 message head, kj::HttpHeaders::serialize()'s
        /// exact output bytes: `word1 word2 word3\r\n` iff `word1` is non-empty, one
        /// `name: value\r\n` per added header, and a final `\r\n` from `finish`.
        type HttpSerializer;
        fn new_http_serializer(word1: &[u8], word2: &[u8], word3: &[u8]) -> Box<HttpSerializer>;
        fn add_header(self: &mut HttpSerializer, name: &[u8], value: &[u8]);
        fn finish(self: &mut HttpSerializer) -> Vec<u8>;

        /// kj's `isValidRequestUrl`: no bytes <= 0x20 (whitespace/controls incl. CR/LF/NUL) and
        /// no 0x7f; bytes >= 0x80 are permitted.
        fn is_valid_request_url(url: &[u8]) -> bool;
        /// kj's `isValidStatusText`: no NUL, CR, or LF.
        fn is_valid_status_text(text: &[u8]) -> bool;

        /// kj's `HttpHeaders::tryParse`: trim the trailing "\r\n" (writing the '\0' sentinel,
        /// kj's `trimHeaderEnding`) and parse the header block IN PLACE, NUL-terminating each
        /// field exactly as kj does.
        fn parse_http_headers(buffer: &mut [u8]) -> HttpHeaderParseResult;
        /// kj's `HttpHeaders::parseHeaders(ptr, end)`: like [`parse_http_headers`] but for an
        /// ALREADY-trimmed buffer whose last byte is the '\0' sentinel (`end` points at it).
        fn parse_http_headers_trimmed(buffer: &mut [u8]) -> HttpHeaderParseResult;

        /// kj's `tryParseHttpRangeHeader` over a `Range` header value (RFC9110 section 14.1).
        fn parse_range_header(value: &[u8], content_length: u64) -> HttpRangesResult;
    }

    // --- kj::HttpService ffi

    unsafe extern "C++" {
        type TlsStarterCallback;
    }

    /// Corresponds to `kj::HttpConnectSettings`.
    struct HttpConnectSettings<'a> {
        use_tls: bool,
        tls_starter: KjMaybe<Pin<&'a mut TlsStarterCallback>>,
    }

    unsafe extern "C++" {
        type AsyncInputStream = crate::io::ffi::AsyncInputStream;
        type AsyncIoStream = crate::io::ffi::AsyncIoStream;
        type AsyncOutputStream = crate::io::ffi::AsyncOutputStream;
        type ConnectResponse;
        type HttpServiceResponse;
        type HttpService;

        fn response_send(
            this_: Pin<&mut HttpServiceResponse>,
            status_code: u32,
            status_text: &str,
            headers: &HttpHeaders,
            expected_body_size: KjMaybe<u64>,
        ) -> Result<KjOwn<AsyncOutputStream>>;

        fn connect_response_accept(
            this_: Pin<&mut ConnectResponse>,
            status_code: u32,
            status_text: &str,
            headers: &HttpHeaders,
        ) -> Result<()>;

        fn connect_response_reject(
            this_: Pin<&mut ConnectResponse>,
            status_code: u32,
            status_text: &str,
            headers: &HttpHeaders,
            expected_body_size: KjMaybe<u64>,
        ) -> Result<KjOwn<AsyncOutputStream>>;

        /// Corresponds to `kj::HttpService::request`.
        async fn request(
            this_: Pin<&mut HttpService>,
            method: HttpMethod,
            url: &[u8],
            headers: &HttpHeaders,
            request_body: Pin<&mut AsyncInputStream>,
            response: Pin<&mut HttpServiceResponse>,
        ) -> Result<()>;

        /// Corresponds to `kj::HttpService::connect`.
        async fn connect(
            this_: Pin<&mut HttpService>,
            host: &[u8],
            headers: &HttpHeaders,
            connection: Pin<&mut AsyncIoStream>,
            response: Pin<&mut ConnectResponse>,
            settings: HttpConnectSettings<'_>,
        ) -> Result<()>;
    }
}

// --- safe wrappers over the borrow-returning bridge getters --------------------------------
//
// The three `ffi::get_header*` shims are `unsafe fn` only because cxx blanket-marks extern fns
// that carry an explicit lifetime; the `'a` on each is correctly tied to the `&'a HttpHeaders`
// input borrow, so wrapping each in a safe `pub(crate) fn` is sound. The wholly-safe `HeadersRef`
// wrappers (headers.rs) call these instead of touching `ffi` directly.

/// Safe wrapper over [`ffi::get_header`]: look up a builtin header value.
pub(crate) fn get_header(headers: &ffi::HttpHeaders, id: ffi::BuiltinIndicesEnum) -> Option<&[u8]> {
    // SAFETY: the returned slice's lifetime is derived from the `&HttpHeaders` input ref, so Rust
    // prevents use-after-free; kj-http's header storage is stable and immutable for the duration
    // of the shared borrow.
    unsafe { ffi::get_header(headers, id) }.into()
}

/// Safe wrapper over [`ffi::get_header_by_id`]: look up a header value by `HttpHeaderId`.
pub(crate) fn get_header_by_id<'a>(
    headers: &'a ffi::HttpHeaders,
    id: &ffi::HttpHeaderId,
) -> Option<&'a [u8]> {
    // SAFETY: the returned slice's `'a` lifetime is derived from the `&'a HttpHeaders` input ref,
    // so Rust prevents use-after-free; kj-http's header storage is stable and immutable for the
    // duration of the shared borrow.
    unsafe { ffi::get_header_by_id(headers, id) }.into()
}

/// Safe wrapper over [`ffi::get_header_by_name`]: look up a header value by name.
pub(crate) fn get_header_by_name<'a>(
    headers: &'a ffi::HttpHeaders,
    name: &str,
) -> Option<&'a [u8]> {
    // SAFETY: the returned slice's `'a` lifetime is derived from the `&'a HttpHeaders` input ref,
    // so Rust prevents use-after-free; kj-http's header storage is stable and immutable for the
    // duration of the shared borrow.
    unsafe { ffi::get_header_by_name(headers, name) }.into()
}

impl<'a> crate::headers::CustomHeaderId<'a> {
    /// Reinterpret a slice of `*const CustomHeader` pointers (as received from C++ via CXX) into
    /// a slice of `CustomHeaderId`.
    ///
    /// This is the canonical way to receive a `kj::ArrayPtr<const kj::HttpHeaderId>` from C++:
    /// the C++ side converts the array into a `rust::Slice<const HttpHeaderId* const>` and the
    /// Rust side calls this function to get a safe `&[CustomHeaderId]`.
    ///
    /// # Safety
    /// `CustomHeaderId` is `#[repr(transparent)]` over `&CustomHeader`, which has
    /// the same layout as `*const kj::HttpHeaderId`. The caller guarantees all pointers are valid.
    pub unsafe fn from_ptr_slice(ptrs: &'a [*const ffi::HttpHeaderId]) -> &'a [Self] {
        let ptr = std::ptr::from_ref::<[*const ffi::HttpHeaderId]>(ptrs) as *const [Self];
        // SAFETY: CustomHeaderId is #[repr(transparent)] over &CustomHeader; caller guarantees all pointers are valid.
        unsafe { &*ptr }
    }
}
