use std::marker::PhantomData;
use std::pin::Pin;

use futures::TryFutureExt;
use kj_rs::KjOwn;
use static_assertions::assert_eq_align;
use static_assertions::assert_eq_size;

use crate::OwnOrRef;
use crate::Result;
use crate::io::AsyncInputStream;
use crate::io::AsyncIoStream;

#[cxx::bridge(namespace = "kj::rust")]
#[expect(clippy::missing_panics_doc)]
#[expect(clippy::missing_safety_doc)]
#[expect(clippy::elidable_lifetime_names)]
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

    // --- HttpHeaders
    // TODO(when needed): support more than builtin headers

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
        type HttpHeaders;
        fn clone_shallow(this_: &HttpHeaders) -> KjOwn<HttpHeaders>;
        fn set_header(this_: Pin<&mut HttpHeaders>, id: BuiltinIndicesEnum, value: &str);
        unsafe fn get_header<'a>(
            this_: &'a HttpHeaders,
            id: BuiltinIndicesEnum,
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
        type AsyncInputStream = crate::io::AsyncInputStream;
        type AsyncIoStream = crate::io::AsyncIoStream;
        type ConnectResponse;
        type HttpServiceResponse;
        type HttpService;

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

/// Non-owning constant reference to `kj::HttpHeaders`
pub struct HttpHeadersRef<'a>(&'a ffi::HttpHeaders);

impl HttpHeadersRef<'_> {
    pub fn get(&self, id: HeaderId) -> Option<&[u8]> {
        unsafe { ffi::get_header(self.0, id).into() }
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
    pub fn set(&mut self, id: HeaderId, value: &str) {
        ffi::set_header(self.own.as_mut(), id, value);
    }

    pub fn as_ref(&'a self) -> HttpHeadersRef<'a> {
        HttpHeadersRef(self.own.as_ref())
    }
}

pub type HttpMethod = ffi::HttpMethod;
pub type HttpServiceResponse = ffi::HttpServiceResponse;
pub type ConnectResponse = ffi::ConnectResponse;
pub type HttpConnectSettings<'a> = ffi::HttpConnectSettings<'a>;

#[async_trait::async_trait(?Send)]
pub trait HttpService {
    /// Make an HTTP request.
    async fn request<'a>(
        &'a mut self,
        method: HttpMethod,
        url: &'a [u8],
        headers: HttpHeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: Pin<&'a mut HttpServiceResponse>,
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
        response: Pin<&'a mut ConnectResponse>,
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

pub struct CxxHttpService<'a>(OwnOrRef<'a, ffi::HttpService>);

#[async_trait::async_trait(?Send)]
impl HttpService for CxxHttpService<'_> {
    async fn request<'a>(
        &'a mut self,
        method: HttpMethod,
        url: &'a [u8],
        headers: HttpHeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: Pin<&'a mut HttpServiceResponse>,
    ) -> Result<()> {
        ffi::request(
            unsafe { self.0.as_mut() },
            method,
            url,
            headers.0,
            request_body,
            response,
        )
        .await?;
        Ok(())
    }

    fn connect<'a, 'b>(
        &'a mut self,
        host: &'a [u8],
        headers: HttpHeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: Pin<&'a mut ConnectResponse>,
        settings: HttpConnectSettings<'a>,
    ) -> ::core::pin::Pin<Box<dyn ::core::future::Future<Output = Result<()>> + 'b>>
    where
        'a: 'b,
        Self: 'b,
    {
        Box::pin(
            ffi::connect(
                unsafe { self.0.as_mut() },
                host,
                headers.0,
                connection,
                response,
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
        response: Pin<&'a mut HttpServiceResponse>,
    ) -> Result<()> {
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
        response: Pin<&'a mut ConnectResponse>,
        settings: HttpConnectSettings<'a>,
    ) -> impl Future<Output = Result<()>> {
        self.0.connect(
            host,
            HttpHeadersRef(headers),
            connection,
            response,
            settings,
        )
    }
}
