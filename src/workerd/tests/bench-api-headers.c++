// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/http.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

// A benchmark for js Header class.

namespace workerd {
namespace {

struct ApiHeaders: public benchmark::Fixture {
  virtual ~ApiHeaders() noexcept(true) {}

  struct Header {
    bool append;
    kj::StringPtr name;
    kj::StringPtr value;
  };

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
    original = kj::mv(in);
  }

  void TearDown(benchmark::State& state) noexcept(true) override {
    fixture = nullptr;
  }

  kj::Maybe<kj::String> original;
  kj::Own<TestFixture> fixture;
  kj::Own<kj::HttpHeaderTable> table;
  kj::Own<kj::HttpHeaders> kjHeaders;
  Header kHeaders[13] = {Header{false, "Host"_kj, "example.com"_kj},
    Header{false, "User-Agent"_kj,
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"_kj},
    Header{false, "Accept"_kj,
      "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8"_kj},
    Header{false, "Accept-Language"_kj, "en-US,en;q=0.9"_kj},
    Header{false, "Accept-Encoding"_kj, "gzip, deflate, br"_kj},
    Header{false, "Content-Type"_kj, "application/json; charset=utf-8"_kj},
    Header{false, "Authorization"_kj,
      "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0"_kj},
    Header{false, "Cache-Control"_kj, "no-cache, no-store, must-revalidate"_kj},
    Header{false, "Content-Length"_kj, "1234"_kj},
    Header{false, "Referer"_kj, "https://www.example.com/page?query=value&other=param"_kj},
    Header{false, "X-Forwarded-For"_kj, "203.0.113.1, 198.51.100.17"_kj},
    Header{true, "Set-Cookie"_kj, "new_session=token123; Path=/; Secure; HttpOnly"_kj},
    Header{true, "Set-Cookie"_kj, "new_session=token124; Path=/abc; Secure; HttpOnly"_kj}};
};

// initialization performs a lot of copying, benchmark it
BENCHMARK_F(ApiHeaders, constructor)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto _: state) {
      for (size_t i = 0; i < 10000; ++i) {
        benchmark::DoNotOptimize(
            js.alloc<api::Headers>(js, *kjHeaders, api::Headers::Guard::REQUEST));
        benchmark::DoNotOptimize(i);
      }
    }
  });
}

BENCHMARK_F(ApiHeaders, set_append)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto _: state) {
      for (size_t i = 0; i < 1000; ++i) {
        auto headers = js.alloc<api::Headers>();
        // Set common headers with various representative lengths
        for (int n = 0; n < 13; n++) {
          auto& h = kHeaders[n];
          if (h.append) {
            headers->append(
                env.js, jsg::ByteString(kj::str(h.name)), jsg::ByteString(kj::str(h.value)));
          } else {
            headers->set(
                env.js, jsg::ByteString(kj::str(h.name)), jsg::ByteString(kj::str(h.value)));
          }
        }
        benchmark::DoNotOptimize(i);
      }
    }
  });
}
}  // namespace
}  // namespace workerd
