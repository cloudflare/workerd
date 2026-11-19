// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge to `ffi.h`: an HTTP request made on a C++ `WorkerInterface` in the shape Rust
//! wants it, the request body as a stream to write and the response as a head and a stream to
//! read. C++ supplies that shape with `kj::newHttpClient(kj::HttpService&)`.
#![allow(
    unsafe_code,
    reason = "holds a cxx bridge, which expands to unsafe FFI glue"
)]

use kj::http::HeadersRef;
use kj::http::Method;
use kj::io::AsyncOutputStream;
use kj_rs::KjOwn;
use worker::WorkerInterface;

use crate::Result;
use crate::pump::Source;

#[cxx::bridge(namespace = "workerd::rust::send_worker")]
mod bridge {
    #[namespace = "kj::rust"]
    unsafe extern "C++" {
        include!("workerd/rust/kj/ffi.h");

        type HttpHeaders = kj::http::ffi::HttpHeaders;
        type HttpMethod = kj::http::ffi::HttpMethod;
        type AsyncOutputStream = kj::io::ffi::AsyncOutputStream;
    }

    #[namespace = "workerd::rust::worker"]
    unsafe extern "C++" {
        include!("workerd/rust/worker/ffi.h");

        type WorkerInterface = worker::ffi::bridge::WorkerInterface;
    }

    unsafe extern "C++" {
        include!("workerd/rust/send-worker/ffi.h");

        /// One HTTP request on a `WorkerInterface`, which it owns.
        type Subrequest;

        /// Starts the request. `headers` and `url` are copied.
        fn subrequest_start(
            worker: KjOwn<WorkerInterface>,
            method: HttpMethod,
            url: &str,
            headers: &HttpHeaders,
            expected_body_size: KjMaybe<u64>,
        ) -> Result<KjOwn<Subrequest>>;

        /// The request body, to write and then drop, which ends it. It is one end of a pipe
        /// and does not refer to the `Subrequest`.
        fn subrequest_take_request_body(
            call: Pin<&mut Subrequest>,
        ) -> Result<KjOwn<AsyncOutputStream>>;

        /// Resolves once the target sent its response head, which the accessors below then
        /// describe.
        async fn subrequest_response(call: Pin<&mut Subrequest>) -> Result<()>;
        fn subrequest_status(call: &Subrequest) -> Result<u32>;
        fn subrequest_status_text(call: &Subrequest) -> Result<String>;
        /// # Safety
        ///
        /// The headers are the response's, which `call` keeps until it is destroyed: the
        /// caller holds them no longer than `call`.
        unsafe fn subrequest_headers<'a>(call: &'a Subrequest) -> Result<&'a HttpHeaders>;
        /// The response body's `tryGetLength()`; none before the response arrived.
        fn subrequest_body_length(call: Pin<&mut Subrequest>) -> KjMaybe<u64>;
        /// The response body's `tryRead()`.
        async fn subrequest_read(
            call: Pin<&mut Subrequest>,
            buffer: &mut [u8],
            min_bytes: usize,
        ) -> Result<usize>;
    }
}

/// One HTTP request on a `WorkerInterface`. Dropping it cancels the request.
pub struct Subrequest(KjOwn<bridge::Subrequest>);

impl Subrequest {
    /// Starts a request on `worker`, which the `Subrequest` owns from here on.
    pub fn start(
        worker: KjOwn<WorkerInterface>,
        method: Method,
        url: &str,
        headers: HeadersRef<'_>,
        expected_body_size: Option<u64>,
    ) -> Result<Self> {
        Ok(Self(bridge::subrequest_start(
            worker,
            method,
            url,
            headers.as_ffi(),
            expected_body_size.into(),
        )?))
    }

    /// The request body: written, then dropped to end it. Taken once.
    pub fn take_request_body(&mut self) -> Result<AsyncOutputStream<'static>> {
        Ok(bridge::subrequest_take_request_body(self.0.as_mut())?.into())
    }

    /// Waits for the response head.
    pub async fn response(&mut self) -> Result<()> {
        Ok(bridge::subrequest_response(self.0.as_mut()).await?)
    }

    pub fn status(&self) -> Result<u32> {
        Ok(bridge::subrequest_status(&self.0)?)
    }

    pub fn status_text(&self) -> Result<String> {
        Ok(bridge::subrequest_status_text(&self.0)?)
    }

    pub fn headers(&self) -> Result<HeadersRef<'_>> {
        // SAFETY: the result borrows `self`, which owns the C++ object whose response holds the
        // headers; nothing replaces the response once it has arrived.
        Ok(HeadersRef::from(unsafe {
            bridge::subrequest_headers(&self.0)
        }?))
    }

    pub fn body_length(&mut self) -> Option<u64> {
        bridge::subrequest_body_length(self.0.as_mut()).into()
    }
}

/// The response body.
impl Source for Subrequest {
    async fn read(&mut self, buffer: &mut [u8]) -> Result<usize> {
        Ok(bridge::subrequest_read(self.0.as_mut(), buffer, 1).await?)
    }
}
