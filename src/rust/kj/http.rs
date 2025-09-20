use std::pin::Pin;

use cxx::KjError;
use kj_rs::KjOwn;

use crate::OwnOrRef;
use crate::io::AsyncInputStream;
use crate::io::AsyncIoStream;

pub type Result<T> = std::result::Result<T, KjError>;

#[cxx::bridge(namespace = "kj::rust")]
#[expect(clippy::missing_panics_doc)]
#[expect(clippy::missing_safety_doc)]
#[expect(clippy::needless_lifetimes)]
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
            url: &str,
            headers: &HttpHeaders,
            request_body: Pin<&mut AsyncInputStream>,
            response: Pin<&mut HttpServiceResponse>,
        ) -> Result<()>;

        /// Corresponds to `kj::HttpService::connect`.
        async fn connect(
            this_: Pin<&mut HttpService>,
            host: &str,
            headers: &HttpHeaders,
            connection: Pin<&mut AsyncIoStream>,
            response: Pin<&mut ConnectResponse>,
            settings: HttpConnectSettings,
        ) -> Result<()>;
    }
}

pub type HeaderId = ffi::BuiltinIndicesEnum;

pub struct HttpHeaders<'a>(OwnOrRef<'a, ffi::HttpHeaders>);

impl HttpHeaders<'_> {
    pub fn set(&mut self, id: HeaderId, value: &str) {
        unsafe { ffi::set_header(self.0.as_mut(), id, value) };
    }

    #[must_use]
    pub fn clone_shallow(&self) -> Self {
        HttpHeaders(ffi::clone_shallow(&self.0).into())
    }

    pub fn get(&self, id: HeaderId) -> Option<&[u8]> {
        unsafe { ffi::get_header(&self.0, id).into() }
    }
}

impl<'a> From<&'a ffi::HttpHeaders> for HttpHeaders<'a> {
    fn from(value: &'a ffi::HttpHeaders) -> Self {
        HttpHeaders(value.into())
    }
}

pub type HttpMethod = ffi::HttpMethod;
pub type HttpServiceResponse = ffi::HttpServiceResponse;
pub type ConnectResponse = ffi::ConnectResponse;
pub type HttpConnectSettings<'a> = ffi::HttpConnectSettings<'a>;

#[async_trait::async_trait(?Send)]
pub trait HttpService {
    /// Make an HTTP request.
    async fn request(
        &mut self,
        method: HttpMethod,
        url: &str,
        headers: &HttpHeaders<'_>,
        request_body: Pin<&mut AsyncInputStream>,
        response: Pin<&mut HttpServiceResponse>,
    ) -> Result<()>;

    /// Make a CONNECT request
    async fn connect(
        &mut self,
        host: &str,
        headers: &HttpHeaders<'_>,
        connection: Pin<&mut AsyncIoStream>,
        response: Pin<&mut ConnectResponse>,
        settings: HttpConnectSettings<'_>,
    ) -> Result<()>;
}

pub struct CxxHttpService<'a>(OwnOrRef<'a, ffi::HttpService>);

#[async_trait::async_trait(?Send)]
impl HttpService for CxxHttpService<'_> {
    async fn request(
        &mut self,
        method: HttpMethod,
        url: &str,
        headers: &HttpHeaders<'_>,
        request_body: Pin<&mut AsyncInputStream>,
        response: Pin<&mut HttpServiceResponse>,
    ) -> Result<()> {
        ffi::request(
            unsafe { self.0.as_mut() },
            method,
            url,
            headers.0.as_ref(),
            request_body,
            response,
        )
        .await?;
        Ok(())
    }

    async fn connect(
        &mut self,
        host: &str,
        headers: &HttpHeaders<'_>,
        connection: Pin<&mut AsyncIoStream>,
        response: Pin<&mut ConnectResponse>,
        settings: HttpConnectSettings<'_>,
    ) -> Result<()> {
        ffi::connect(
            unsafe { self.0.as_mut() },
            host,
            headers.0.as_ref(),
            connection,
            response,
            settings,
        )
        .await?;
        Ok(())
    }
}

impl From<KjOwn<ffi::HttpService>> for CxxHttpService<'_> {
    fn from(value: KjOwn<ffi::HttpService>) -> Self {
        CxxHttpService(value.into())
    }
}
