use std::marker::PhantomData;
use std::pin::Pin;

use futures::TryFutureExt;
use kj_rs::KjOwn;
use static_assertions::assert_eq_align;
use static_assertions::assert_eq_size;

use crate::OwnOrMut;
use crate::Result;
use crate::io::AsyncInputStream;
use crate::io::AsyncIoStream;

#[cxx::bridge(namespace = "kj::rust")]
#[expect(clippy::missing_panics_doc)]
#[expect(clippy::missing_safety_doc)]
pub mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/kj/ffi.h");
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

    unsafe extern "C++" {
        type BuiltinIndicesEnum;
        type HttpHeaderTable;
        type HttpHeaders;
        fn new_http_headers(table: &HttpHeaderTable) -> KjOwn<HttpHeaders>;
        fn clone_shallow(this_: &HttpHeaders) -> KjOwn<HttpHeaders>;
        fn set_header(this_: Pin<&mut HttpHeaders>, id: BuiltinIndicesEnum, value: &str);
        unsafe fn get_header<'a>(
            this_: &'a HttpHeaders,
            id: BuiltinIndicesEnum,
        ) -> KjMaybe<&'a [u8]>;
        unsafe fn get_header_by_id<'a>(
            this_: &'a HttpHeaders,
            id: &HttpHeaderId,
        ) -> KjMaybe<&'a [u8]>;
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

    // DynHttpService
    extern "Rust" {
        type DynHttpService;

        async unsafe fn request<'a>(
            self: &'a mut DynHttpService,
            method: HttpMethod,
            url: &'a [u8],
            headers: &'a HttpHeaders,
            request_body: Pin<&'a mut AsyncInputStream>,
            response: Pin<&'a mut HttpServiceResponse>,
        ) -> Result<()>;

        async unsafe fn connect<'a>(
            self: &'a mut DynHttpService,
            host: &'a [u8],
            headers: &'a HttpHeaders,
            connection: Pin<&'a mut AsyncIoStream>,
            response: Pin<&'a mut ConnectResponse>,
            settings: HttpConnectSettings<'a>,
        ) -> Result<()>;
    }

    impl Box<DynHttpService> {}
}

assert_eq_size!(ffi::HttpConnectSettings, [u8; 16]);
assert_eq_align!(ffi::HttpConnectSettings, u64);

pub type HeaderId = ffi::BuiltinIndicesEnum;
pub type HttpHeaderTable = ffi::HttpHeaderTable;
pub type CustomHttpHeader = ffi::HttpHeaderId;
// TODO(tewaro) soon: replace by enum HeaderId

/// Non-owning reference to a `kj::HttpHeaderId`.
///
/// `CustomHttpHeader` is an opaque CXX type representing `HttpHeaderId` and can only be passed by
/// reference across the FFI boundary. This wrapper makes the borrow lifetime explicit and provides
/// a safe Rust handle.
///
/// `repr(transparent)` guarantees the same layout as `&ffi::HttpHeaderId` (i.e. a single
/// pointer), which allows safe reinterpretation of `&[*const HttpHeaderId]` slices received
/// from C++ into `&[CustomHttpHeaderId]` via [`CustomHttpHeaderId::from_ptr_slice`].
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct CustomHttpHeaderId<'a>(&'a CustomHttpHeader);

impl<'a> From<&'a CustomHttpHeader> for CustomHttpHeaderId<'a> {
    fn from(value: &'a CustomHttpHeader) -> Self {
        CustomHttpHeaderId(value)
    }
}

impl<'a> CustomHttpHeaderId<'a> {
    /// Reinterpret a slice of `*const CustomHttpHeader` pointers (as received from C++ via CXX) into
    /// a slice of `HttpHeader`.
    ///
    /// This is the canonical way to receive a `kj::ArrayPtr<const kj::HttpHeaderId>` from C++:
    /// the C++ side converts the array into a `rust::Slice<const HttpHeaderId* const>` and the
    /// Rust side calls this function to get a safe `&[HttpHeaderIdRef]`.
    ///
    /// # Safety
    /// `CustomHttpHeaderId` is `#[repr(transparent)]` over `&CustomHttpHeader`, which has
    /// the same layout as `*const kj::HttpHeaderId`. The caller guarantees all pointers are valid.
    pub unsafe fn from_ptr_slice(ptrs: &'a [*const CustomHttpHeader]) -> &'a [Self] {
        let ptr = std::ptr::from_ref::<[*const CustomHttpHeader]>(ptrs) as *const [Self];
        // SAFETY: CustomHttpHeaderId is #[repr(transparent)] over &CustomHttpHeader; caller guarantees all pointers are valid.
        unsafe { &*ptr }
    }
}

/// Non-owning constant reference to `kj::HttpHeaders`
#[derive(Clone, Copy)]
pub struct HttpHeadersRef<'a>(&'a ffi::HttpHeaders);

impl HttpHeadersRef<'_> {
    pub fn get(&self, id: HeaderId) -> Option<&[u8]> {
        // SAFETY: self.0 is a valid HttpHeaders reference and id is a valid builtin header enum.
        unsafe { ffi::get_header(self.0, id).into() }
    }

    /// Look up a header by its `kj::HttpHeaderId`. This works for both builtin headers and custom
    /// headers registered via `HttpHeaderTable::Builder::add()`.
    pub fn get_by_id(&self, id: CustomHttpHeaderId<'_>) -> Option<&[u8]> {
        // SAFETY: self.0 is a valid HttpHeaders reference and id.0 is a valid HttpHeaderId.
        unsafe { ffi::get_header_by_id(self.0, id.0).into() }
    }

    #[must_use]
    pub fn clone_shallow(&self) -> HttpHeaders<'_> {
        HttpHeaders {
            own: ffi::clone_shallow(self.0),
            _marker: PhantomData,
        }
    }
}

impl<'a> From<&'a ffi::HttpHeaders> for HttpHeadersRef<'a> {
    fn from(value: &'a ffi::HttpHeaders) -> Self {
        HttpHeadersRef(value)
    }
}

/// `HttpHeaders` that `kj::Own` the underlying C++ header object.
///
/// Notice, that despite the fact that headers are fully owned, because of a `shallowClone`
/// method, data might not be owned: hence the lifetime parameter.
pub struct HttpHeaders<'a> {
    own: KjOwn<ffi::HttpHeaders>,
    _marker: PhantomData<&'a ffi::HttpHeaders>,
}

impl<'a> HttpHeaders<'a> {
    #[must_use]
    pub fn new(table: &'a HttpHeaderTable) -> Self {
        Self {
            own: ffi::new_http_headers(table),
            _marker: PhantomData,
        }
    }

    pub fn set(&mut self, id: HeaderId, value: &str) {
        ffi::set_header(self.own.as_mut(), id, value);
    }

    pub fn as_ref(&'a self) -> HttpHeadersRef<'a> {
        HttpHeadersRef(self.own.as_ref())
    }
}

impl<'a, 'b> From<&'b HttpHeaders<'a>> for HttpHeadersRef<'b> {
    fn from(value: &'b HttpHeaders<'a>) -> Self {
        value.as_ref()
    }
}

pub type HttpMethod = ffi::HttpMethod;
pub type HttpConnectSettings<'a> = ffi::HttpConnectSettings<'a>;

/// Non-owning mutable reference to `kj::HttpService::Response`.
pub struct HttpServiceResponse<'a>(Pin<&'a mut ffi::HttpServiceResponse>);

impl<'a> HttpServiceResponse<'a> {
    /// Send response metadata and obtain the writable response body stream.
    pub fn send<'h>(
        self,
        status_code: u32,
        status_text: &str,
        headers: impl Into<HttpHeadersRef<'h>>,
        expected_body_size: Option<u64>,
    ) -> Result<crate::io::AsyncOutputStream<'a>> {
        Ok(ffi::response_send(
            self.0,
            status_code,
            status_text,
            headers.into().0,
            expected_body_size.into(),
        )?
        .into())
    }

    pub(crate) fn into_ffi(self) -> Pin<&'a mut ffi::HttpServiceResponse> {
        self.0
    }
}

impl<'a> From<Pin<&'a mut ffi::HttpServiceResponse>> for HttpServiceResponse<'a> {
    fn from(value: Pin<&'a mut ffi::HttpServiceResponse>) -> Self {
        Self(value)
    }
}

/// Non-owning mutable reference to `kj::HttpService::ConnectResponse`.
pub struct ConnectResponse<'a>(Pin<&'a mut ffi::ConnectResponse>);

impl<'a> ConnectResponse<'a> {
    /// Accept the CONNECT request without a response body.
    pub fn accept<'h>(
        self,
        status_code: u32,
        status_text: &str,
        headers: impl Into<HttpHeadersRef<'h>>,
    ) -> Result<()> {
        Ok(ffi::connect_response_accept(
            self.0,
            status_code,
            status_text,
            headers.into().0,
        )?)
    }

    /// Reject the CONNECT request and obtain the writable rejection body stream.
    pub fn reject<'h>(
        self,
        status_code: u32,
        status_text: &str,
        headers: impl Into<HttpHeadersRef<'h>>,
        expected_body_size: Option<u64>,
    ) -> Result<crate::io::AsyncOutputStream<'a>> {
        Ok(ffi::connect_response_reject(
            self.0,
            status_code,
            status_text,
            headers.into().0,
            expected_body_size.into(),
        )?
        .into())
    }

    pub(crate) fn into_ffi(self) -> Pin<&'a mut ffi::ConnectResponse> {
        self.0
    }
}

impl<'a> From<Pin<&'a mut ffi::ConnectResponse>> for ConnectResponse<'a> {
    fn from(value: Pin<&'a mut ffi::ConnectResponse>) -> Self {
        Self(value)
    }
}

#[async_trait::async_trait(?Send)]
pub trait HttpService {
    /// Make an HTTP request.
    async fn request<'a>(
        &'a mut self,
        method: HttpMethod,
        url: &'a [u8],
        headers: HttpHeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: HttpServiceResponse<'a>,
    ) -> Result<()>;

    /// Make a CONNECT request
    ///
    /// WARNING: as c++ implementation does, this method has an outbound _immediate_
    /// parameter inside `settings` (`tls_starter`).
    ///
    /// This method should be implement without using `async` method, since its body is called only
    /// when it is first polled, but by manually creating a future using async block.
    async fn connect<'a>(
        &'a mut self,
        host: &'a [u8],
        headers: HttpHeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: ConnectResponse<'a>,
        settings: HttpConnectSettings<'a>,
    ) -> Result<()>;

    /// Convert `self` into the structure suitable for passing to C++ through FFI layer
    /// To obtain `kj::HttpService` on C++ side finish wrapping with `fromRust()`
    fn into_ffi(self) -> Box<DynHttpService>
    where
        Self: Sized + 'static,
    {
        Box::new(DynHttpService(Box::new(self)))
    }
}

pub struct CxxHttpService<'a>(OwnOrMut<'a, ffi::HttpService>);

#[async_trait::async_trait(?Send)]
impl HttpService for CxxHttpService<'_> {
    async fn request<'a>(
        &'a mut self,
        method: HttpMethod,
        url: &'a [u8],
        headers: HttpHeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: HttpServiceResponse<'a>,
    ) -> Result<()> {
        let service = self.0.as_mut();
        ffi::request(
            service,
            method,
            url,
            headers.0,
            request_body,
            response.into_ffi(),
        )
        .await?;
        Ok(())
    }

    fn connect<'a, 'b>(
        &'a mut self,
        host: &'a [u8],
        headers: HttpHeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: ConnectResponse<'a>,
        settings: HttpConnectSettings<'a>,
    ) -> ::core::pin::Pin<Box<dyn ::core::future::Future<Output = Result<()>> + 'b>>
    where
        'a: 'b,
        Self: 'b,
    {
        let service = self.0.as_mut();
        Box::pin(
            ffi::connect(
                service,
                host,
                headers.0,
                connection,
                response.into_ffi(),
                settings,
            )
            .map_err(Into::into),
        )
    }
}

impl From<KjOwn<ffi::HttpService>> for CxxHttpService<'_> {
    fn from(value: KjOwn<ffi::HttpService>) -> Self {
        CxxHttpService(value.into())
    }
}

pub struct DynHttpService(Box<dyn HttpService>);

impl DynHttpService {
    async fn request<'a>(
        &'a mut self,
        method: HttpMethod,
        url: &'a [u8],
        headers: &'a ffi::HttpHeaders,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: Pin<&'a mut ffi::HttpServiceResponse>,
    ) -> Result<()> {
        let response = HttpServiceResponse::from(response);
        self.0
            .request(method, url, HttpHeadersRef(headers), request_body, response)
            .await?;
        Ok(())
    }

    fn connect<'a>(
        &'a mut self,
        host: &'a [u8],
        headers: &'a ffi::HttpHeaders,
        connection: Pin<&'a mut AsyncIoStream>,
        response: Pin<&'a mut ffi::ConnectResponse>,
        settings: HttpConnectSettings<'a>,
    ) -> impl Future<Output = Result<()>> {
        let headers = HttpHeadersRef(headers);
        let response = ConnectResponse::from(response);
        self.0
            .connect(host, headers, connection, response, settings)
    }
}
