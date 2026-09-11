//! Wholly-safe wrappers for `kj::HttpService` and its response handles: the `Service` trait, the
//! `CxxService` adapter over a C++ `kj::HttpService`, `ServiceResponse`, `ConnectResponse`, and the
//! `Method` / `ConnectSettings` aliases.
//!
//! This file inherits the crate-root `#![deny(unsafe_code)]` — it carries no `#![allow(unsafe_code)]`
//! and contains ZERO `unsafe`. Every FFI call here is a safe bridge shim exposed by the `http`
//! island (`crate::http::ffi::{response_send, connect_response_accept, connect_response_reject,
//! request, connect}`). Header handles are passed to FFI via the safe `HeadersRef::as_ffi()`
//! accessor. Re-exported from `crate::http` so the `kj::http::{Service, CxxService, ...}` public
//! paths are unchanged.

use std::pin::Pin;

use futures::TryFutureExt;
use kj_rs::KjOwn;
use static_assertions::assert_eq_align;
use static_assertions::assert_eq_size;

use crate::OwnOrMut;
use crate::Result;
use crate::http::HeadersRef;
use crate::http::ffi;
use crate::io::AsyncInputStream;
use crate::io::AsyncIoStream;

assert_eq_size!(ffi::HttpConnectSettings, [u8; 16]);
assert_eq_align!(ffi::HttpConnectSettings, u64);

pub type Method = ffi::HttpMethod;
pub type ConnectSettings<'a> = ffi::HttpConnectSettings<'a>;

/// Non-owning mutable reference to `kj::HttpService::Response`.
pub struct ServiceResponse<'a>(Pin<&'a mut ffi::HttpServiceResponse>);

impl<'a> ServiceResponse<'a> {
    /// Send response metadata and obtain the writable response body stream.
    pub fn send<'h>(
        self,
        status_code: u32,
        status_text: &str,
        headers: impl Into<HeadersRef<'h>>,
        expected_body_size: Option<u64>,
    ) -> Result<crate::io::AsyncOutputStream<'a>> {
        Ok(ffi::response_send(
            self.0,
            status_code,
            status_text,
            headers.into().as_ffi(),
            expected_body_size.into(),
        )?
        .into())
    }

    pub fn into_ffi(self) -> Pin<&'a mut ffi::HttpServiceResponse> {
        self.0
    }
}

impl<'a> From<Pin<&'a mut ffi::HttpServiceResponse>> for ServiceResponse<'a> {
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
        headers: impl Into<HeadersRef<'h>>,
    ) -> Result<()> {
        Ok(ffi::connect_response_accept(
            self.0,
            status_code,
            status_text,
            headers.into().as_ffi(),
        )?)
    }

    /// Reject the CONNECT request and obtain the writable rejection body stream.
    pub fn reject<'h>(
        self,
        status_code: u32,
        status_text: &str,
        headers: impl Into<HeadersRef<'h>>,
        expected_body_size: Option<u64>,
    ) -> Result<crate::io::AsyncOutputStream<'a>> {
        Ok(ffi::connect_response_reject(
            self.0,
            status_code,
            status_text,
            headers.into().as_ffi(),
            expected_body_size.into(),
        )?
        .into())
    }

    pub fn into_ffi(self) -> Pin<&'a mut ffi::ConnectResponse> {
        self.0
    }
}

impl<'a> From<Pin<&'a mut ffi::ConnectResponse>> for ConnectResponse<'a> {
    fn from(value: Pin<&'a mut ffi::ConnectResponse>) -> Self {
        Self(value)
    }
}

#[async_trait::async_trait(?Send)]
pub trait Service {
    /// Make an HTTP request.
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
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
        headers: HeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: ConnectResponse<'a>,
        settings: ConnectSettings<'a>,
    ) -> Result<()>;
}

pub struct CxxService<'a>(OwnOrMut<'a, ffi::HttpService>);

#[async_trait::async_trait(?Send)]
impl Service for CxxService<'_> {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let service = self.0.as_mut();
        ffi::request(
            service,
            method,
            url,
            headers.as_ffi(),
            request_body,
            response.into_ffi(),
        )
        .await?;
        Ok(())
    }

    fn connect<'a, 'b>(
        &'a mut self,
        host: &'a [u8],
        headers: HeadersRef<'a>,
        connection: Pin<&'a mut AsyncIoStream>,
        response: ConnectResponse<'a>,
        settings: ConnectSettings<'a>,
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
                headers.as_ffi(),
                connection,
                response.into_ffi(),
                settings,
            )
            .map_err(Into::into),
        )
    }
}

impl From<KjOwn<ffi::HttpService>> for CxxService<'_> {
    fn from(value: KjOwn<ffi::HttpService>) -> Self {
        CxxService(value.into())
    }
}

impl<'a> From<Pin<&'a mut ffi::HttpService>> for CxxService<'a> {
    fn from(value: Pin<&'a mut ffi::HttpService>) -> Self {
        CxxService(value.into())
    }
}
