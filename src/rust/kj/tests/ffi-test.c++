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

KJ_TEST("get header by id round-trip through Rust") {
  // Register a custom header and a second one we'll leave unset.
  kj::HttpHeaderTable::Builder builder;
  auto customId = builder.add("X-Custom-Header");
  auto absentId = builder.add("X-Absent-Header");
  auto table = builder.build();

  kj::HttpHeaders headers(*table);
  headers.setPtr(customId, "hello-from-cpp");

  // Also set a builtin header to test that path.
  headers.setPtr(kj::HttpHeaderId::HOST, "example.com");

  // Round-trip: C++ -> Rust (get_header_value_via_id) -> C++ (get_header_by_id shim) -> value.
  {
    auto maybe = kj::rust::tests::get_header_value_via_id(headers, customId);
    KJ_IF_SOME(value, maybe) {
      auto strValue = kj::StringPtr(reinterpret_cast<const char*>(value.data()), value.size());
      KJ_EXPECT(strValue == "hello-from-cpp", strValue);
    } else {
      KJ_FAIL_EXPECT("expected Some for custom header, got None");
    }
  }

  // Absent header should return None.
  {
    auto maybe = kj::rust::tests::get_header_value_via_id(headers, absentId);
    KJ_EXPECT(maybe == kj::none, "expected None for absent header");
  }

  // Builtin header via HttpHeaderId.
  {
    auto maybe = kj::rust::tests::get_header_value_via_id(headers, kj::HttpHeaderId::HOST);
    KJ_IF_SOME(value, maybe) {
      auto strValue = kj::StringPtr(reinterpret_cast<const char*>(value.data()), value.size());
      KJ_EXPECT(strValue == "example.com", strValue);
    } else {
      KJ_FAIL_EXPECT("expected Some for HOST header, got None");
    }
  }
}

KJ_TEST("assert header ids present via ArrayPtr round-trip through Rust") {
  kj::HttpHeaderTable::Builder builder;
  auto custom1 = builder.add("X-First");
  auto custom2 = builder.add("X-Second");
  auto table = builder.build();

  kj::HttpHeaders headers(*table);
  headers.setPtr(custom1, "value1");
  headers.setPtr(custom2, "value2");
  headers.setPtr(kj::HttpHeaderId::HOST, "example.com");

  // Build an array of pointers from our HttpHeaderIds, simulating what you'd get from
  // kj::ArrayPtr<const kj::HttpHeaderId> — CXX can't pass opaque types in slices directly,
  // so we pass pointers instead.
  const kj::HttpHeaderId* const idPtrs[] = {&custom1, &custom2, &kj::HttpHeaderId::HOST};
  rust::Slice<const kj::HttpHeaderId* const> idSlice(idPtrs, 3);

  // The Rust side wraps each pointer in HttpHeaderIdRef and asserts all are present.
  kj::rust::tests::assert_header_ids_present(headers, idSlice);
}

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
