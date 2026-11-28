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

    // A request the way a browser sends it, plus headers a proxy in front commonly adds.
    browserHeaders = kj::heap<kj::HttpHeaders>(*table);
    auto browser = kj::heapString(
        "GET /index.html HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "Connection: keep-alive\r\n"
        "sec-ch-ua: \"Chromium\";v=\"124\", \"Google Chrome\";v=\"124\", \"Not-A.Brand\";v=\"99\"\r\n"
        "sec-ch-ua-mobile: ?0\r\n"
        "sec-ch-ua-platform: \"macOS\"\r\n"
        "Upgrade-Insecure-Requests: 1\r\n"
        "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8\r\n"
        "Sec-Fetch-Site: same-origin\r\n"
        "Sec-Fetch-Mode: navigate\r\n"
        "Sec-Fetch-User: ?1\r\n"
        "Sec-Fetch-Dest: document\r\n"
        "Referer: https://www.example.com/\r\n"
        "Accept-Encoding: gzip, deflate, br, zstd\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Cookie: session=abcdef0123456789; theme=dark; _ga=GA1.1.123456789.1700000000\r\n"
        "If-None-Match: \"5f3e-1234567890abcdef\"\r\n"
        "X-Forwarded-For: 203.0.113.7\r\n"
        "X-Forwarded-Proto: https\r\n"
        "X-Real-IP: 203.0.113.7\r\n"
        "CF-Connecting-IP: 203.0.113.7\r\n"
        "CF-IPCountry: US\r\n"
        "CF-Ray: 8a1b2c3d4e5f6a7b\r\n"
        "CF-Visitor: {\"scheme\":\"https\"}\r\n"
        "\r\n");
    KJ_EXPECT(browserHeaders->tryParseRequest(browser.asArray()).is<kj::HttpHeaders::Request>());
    browserText = kj::mv(browser);
  }

  void TearDown(benchmark::State& state) noexcept(true) override {
    fixture = nullptr;
  }

  kj::Maybe<kj::String> original;
  kj::Own<TestFixture> fixture;
  kj::Own<kj::HttpHeaderTable> table;
  kj::Own<kj::HttpHeaders> kjHeaders;
  kj::Maybe<kj::String> browserText;
  kj::Own<kj::HttpHeaders> browserHeaders;
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

// The same with a larger, browser-like request.
BENCHMARK_F(ApiHeaders, constructor_browser)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto _: state) {
      for (size_t i = 0; i < 10000; ++i) {
        benchmark::DoNotOptimize(
            js.alloc<api::Headers>(js, *browserHeaders, api::Headers::Guard::REQUEST));
      }
    }
  });
}

// A worker that looks at a few request headers.
BENCHMARK_F(ApiHeaders, construct_get3_browser)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto _: state) {
      for (size_t i = 0; i < 10000; ++i) {
        auto headers = js.alloc<api::Headers>(js, *browserHeaders, api::Headers::Guard::REQUEST);
        benchmark::DoNotOptimize(headers->get(js, kj::str("user-agent")));
        benchmark::DoNotOptimize(headers->get(js, kj::str("CF-Connecting-IP")));
        benchmark::DoNotOptimize(headers->has(kj::str("cookie")));
      }
    }
  });
}

// A worker that iterates over all request headers.
BENCHMARK_F(ApiHeaders, construct_entries_browser)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto _: state) {
      for (size_t i = 0; i < 10000; ++i) {
        auto headers = js.alloc<api::Headers>(js, *browserHeaders, api::Headers::Guard::REQUEST);
        benchmark::DoNotOptimize(headers->getDisplayedHeaders(js));
      }
    }
  });
}

// A proxying worker: `new Request(request)` copies the headers, then fetch() sends them on.
BENCHMARK_F(ApiHeaders, construct_copy_send_browser)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto _: state) {
      for (size_t i = 0; i < 10000; ++i) {
        auto headers = js.alloc<api::Headers>(js, *browserHeaders, api::Headers::Guard::REQUEST);
        auto copy = js.alloc<api::Headers>(js, *headers);
        // Send through the runtime's own header table, as fetch() does.
        kj::HttpHeaders out(env.context.getHeaderTable());
        copy->shallowCopyTo(out);
        benchmark::DoNotOptimize(out.size());
      }
    }
  });
}

// The same, changing one header first.
BENCHMARK_F(ApiHeaders, construct_copy_set_send_browser)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto _: state) {
      for (size_t i = 0; i < 10000; ++i) {
        auto headers = js.alloc<api::Headers>(js, *browserHeaders, api::Headers::Guard::REQUEST);
        auto copy = js.alloc<api::Headers>(js, *headers);
        copy->set(js, kj::str("X-Proxied"), kj::str("1"));
        // Send through the runtime's own header table, as fetch() does.
        kj::HttpHeaders out(env.context.getHeaderTable());
        copy->shallowCopyTo(out);
        benchmark::DoNotOptimize(out.size());
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
        for (auto& h: kHeaders) {
          if (h.append) {
            headers->append(env.js, kj::str(h.name), kj::str(h.value));
          } else {
            headers->set(env.js, kj::str(h.name), kj::str(h.value));
          }
        }
        benchmark::DoNotOptimize(i);
      }
    }
  });
}
}  // namespace
}  // namespace workerd
