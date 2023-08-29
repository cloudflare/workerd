// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/api/http.h>

// A benchmark for js Header class.

namespace workerd {
namespace {

struct ApiHeaders: public benchmark::Fixture {
  virtual ~ApiHeaders() noexcept(true) {}


  void SetUp(benchmark::State& state) noexcept(true) override {
    fixture = kj::heap<TestFixture>();

    kj::HttpHeaderTable::Builder builder;
    builder.add("Host");
    builder.add("Accept");
    builder.add("Content-Type");
    builder.add("Last-Modified");
    table = builder.build();
    kjHeaders = kj::heap<kj::HttpHeaders>(*table);
    auto in = kj::heapString(
          "GET /favicon.ico HTTP/1.1\r\n"
          "Host: 0.0.0.0=5000\r\n"
          "User-Agent: Mozilla/5.0 (X11; U; Linux i686; en-US; rv:1.9) Gecko/2008061015 Firefox/3.0\r\n"
          "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
          "Accept-Language: en-us,en;q=0.5\r\n"
          "Accept-Encoding: gzip,deflate\r\n"
          "Accept-Charset: ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n"
          "Keep-Alive: 300\r\n"
          "Connection: keep-alive\r\n"
          "\r\n");
    KJ_EXPECT(kjHeaders->tryParseRequest(in.asArray()).is<kj::HttpHeaders::Request>());
  }

  void TearDown(benchmark::State& state) noexcept(true) override {
    fixture = nullptr;
  }

  kj::Own<TestFixture> fixture;
  kj::Own<kj::HttpHeaderTable> table;
  kj::Own<kj::HttpHeaders> kjHeaders;
};

// initialization performs a lot of copying, benchmark it
BENCHMARK_F(ApiHeaders, constructor)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    for (auto _ : state) {
      auto jsHeaders = jsg::alloc<api::Headers>(*kjHeaders, api::Headers::Guard::REQUEST);
    }
  });
}

} // namespace
} // namespace workerd
