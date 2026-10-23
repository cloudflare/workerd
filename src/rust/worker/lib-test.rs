// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use kj::http::HeaderId;

#[expect(dead_code, reason = "HeaderOverrideProxy exists as compilation test.")]
struct HeaderOverrideProxy(Box<dyn crate::Interface>);

#[async_trait::async_trait(?Send)]
impl kj::http::Service for HeaderOverrideProxy {
    async fn request<'a>(
        &'a mut self,
        method: kj::http::Method,
        url: &'a [u8],
        headers: kj::http::HeadersRef<'a>,
        request_body: std::pin::Pin<&'a mut kj::io::AsyncInputStream>,
        response: kj::http::ServiceResponse<'a>,
    ) -> kj::Result<()> {
        let mut headers = headers.clone_shallow();
        headers.set(HeaderId::HOST, "example.com");
        self.0
            .request(method, url, headers.as_ref(), request_body, response)
            .await?;
        Ok(())
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: kj::http::HeadersRef<'a>,
        _connection: std::pin::Pin<&'a mut kj::io::AsyncIoStream>,
        _response: kj::http::ConnectResponse<'a>,
        _settings: kj::http::ConnectSettings<'a>,
    ) -> kj::Result<()> {
        todo!()
    }
}
