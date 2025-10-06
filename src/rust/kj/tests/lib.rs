use std::pin::Pin;

use kj::Result;
use kj::http::ConnectResponse;
use kj::http::CxxHttpService;
use kj::http::DynHttpService;
use kj::http::HttpConnectSettings;
use kj::http::HttpHeadersRef;
use kj::http::HttpMethod;
use kj::http::HttpService;
use kj::http::HttpServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjOwn;

#[cxx::bridge(namespace = "kj::rust::tests")]
pub mod ffi {
    #[namespace = "kj::rust"]
    unsafe extern "C++" {
        include!("workerd/rust/kj/ffi.h");
        type HttpService = kj::http::ffi::HttpService;
    }

    #[namespace = "kj::rust"]
    extern "Rust" {
        type DynHttpService = kj::http::DynHttpService;
    }

    extern "Rust" {
        type ProxyHttpService;

        #[expect(clippy::unnecessary_box_returns)]
        fn new_proxy_http_service(service: KjOwn<HttpService>) -> Box<DynHttpService>;
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
