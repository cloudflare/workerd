use std::pin::Pin;

use kj::Result;
use kj::http::ConnectResponse;
use kj::http::CustomHttpHeaderId;
use kj::http::CxxHttpService;
use kj::http::DynHttpService;
use kj::http::HttpConnectSettings;
use kj::http::HttpHeadersRef;
use kj::http::HttpMethod;
use kj::http::HttpService;
use kj::http::HttpServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;

#[cxx::bridge(namespace = "kj::rust::tests")]
pub mod ffi {
    #[namespace = "kj::rust"]
    unsafe extern "C++" {
        include!("workerd/rust/kj/ffi.h");
        type HttpService = kj::http::ffi::HttpService;
        type HttpHeaders = kj::http::ffi::HttpHeaders;
        type HttpHeaderId = kj::http::ffi::HttpHeaderId;
    }

    #[namespace = "kj::rust"]
    extern "Rust" {
        type DynHttpService = kj::http::DynHttpService;
    }

    extern "Rust" {
        type ProxyHttpService;

        #[expect(clippy::unnecessary_box_returns)]
        fn new_proxy_http_service(service: KjOwn<HttpService>) -> Box<DynHttpService>;

        /// Look up a header value by HttpHeaderId, returning the value if present.
        /// This exercises the C++ -> Rust -> C++ round-trip for HttpHeaderId.
        unsafe fn get_header_value_via_id<'a>(
            headers: &'a HttpHeaders,
            id: &HttpHeaderId,
        ) -> KjMaybe<&'a [u8]>;

        /// Receive an array of HttpHeaderIdpointers, convert to &[HttpHeaderIdRef] via
        /// from_ptr_slice, look up each header, and assert all are present.
        /// This exercises passing a kj::ArrayPtr<const kj::HttpHeaderId> into Rust.
        unsafe fn assert_header_ids_present(headers: &HttpHeaders, ids: &[*const HttpHeaderId]);
    }
}

struct ProxyHttpService {
    target: CxxHttpService<'static>,
}

#[async_trait::async_trait(?Send)]
impl HttpService for ProxyHttpService {
    async fn request<'a>(
        &'a mut self,
        method: HttpMethod,
        url: &'a [u8],
        headers: HttpHeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: Pin<&'a mut HttpServiceResponse>,
    ) -> Result<()> {
        self.target
            .request(method, url, headers, request_body, response)
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
            self.target
                .connect(host, headers, connection, response, settings),
        )
    }
}

#[expect(clippy::unnecessary_box_returns)]
fn new_proxy_http_service(service: KjOwn<ffi::HttpService>) -> Box<DynHttpService> {
    ProxyHttpService {
        target: service.into(),
    }
    .into_ffi()
}

fn get_header_value_via_id<'a>(
    headers: &'a ffi::HttpHeaders,
    id: &ffi::HttpHeaderId,
) -> KjMaybe<&'a [u8]> {
    // SAFETY: headers is a valid HttpHeaders ref and id is a valid HttpHeaderId from C++.
    unsafe { kj::http::ffi::get_header_by_id(headers, id) }
}

/// # Safety
///
/// Each pointer in `ids` must be non-null and point to a valid, live `HttpHeaderId`.
unsafe fn assert_header_ids_present(headers: &ffi::HttpHeaders, ids: &[*const ffi::HttpHeaderId]) {
    let headers_ref = HttpHeadersRef::from(headers);
    // SAFETY: All pointers in ids are valid, as guaranteed by the unsafe fn contract.
    let id_refs = unsafe { CustomHttpHeaderId::from_ptr_slice(ids) };
    for (i, &id_ref) in id_refs.iter().enumerate() {
        let value = headers_ref.get_by_id(id_ref);
        assert!(
            value.is_some(),
            "expected header at index {i} to be present, but got None"
        );
    }
}
