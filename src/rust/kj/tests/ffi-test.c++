// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/rust/kj/http.rs.h>
#include <workerd/rust/kj/tests/lib.rs.h>

#include <kj-rs/kj-rs.h>

#include <kj/test.h>

#include <cmath>

using namespace kj_rs;

static kj::StringPtr tlsHost;

kj::Promise<void> startTls(kj::StringPtr hostName) {
  tlsHost = hostName;
  return kj::READY_NOW;
}

class MockHttpService: public kj::HttpService {
 public:
  virtual ~MockHttpService() {}
  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    KJ_UNIMPLEMENTED("not exercised by test");
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    if (settings.useTls) {
      return kj::READY_NOW;
    }
    KJ_IF_SOME(tlsStarter, settings.tlsStarter) {
      tlsStarter = startTls;
    }
    return kj::READY_NOW;
  }
};

class TestConnectResponse: public kj::HttpService::ConnectResponse {
 public:
  void accept(uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {
    KJ_UNIMPLEMENTED("not exercised by test");
  }
  kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    KJ_UNIMPLEMENTED("not exercised by test");
  }
};

KJ_TEST("http connect settings") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto mock = kj::heap<MockHttpService>();
  auto mockRef = *mock;

  auto proxy = kj::rust::tests::new_proxy_http_service(kj::mv(mock));

  kj::StringPtr host = "example.com";
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);

  auto pipe = kj::newTwoWayPipe();
  kj::AsyncIoStream& connection = *pipe.ends[0];

  TestConnectResponse tunnel;

  kj::Own<kj::TlsStarterCallback> tlsStarter = kj::heap<kj::TlsStarterCallback>();
  ::kj::rust::HttpConnectSettings settings = {.use_tls = false, .tls_starter = kj::none};
  settings.tls_starter = tlsStarter;

  auto promise = proxy->connect(host.asBytes().as<Rust>(), headers, connection, tunnel, settings);
  KJ_ASSERT_NONNULL (*tlsStarter)(host).wait(waitScope);
  KJ_EXPECT(tlsHost == host);
}
