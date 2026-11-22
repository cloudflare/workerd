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

// A request as a browser navigation sends it, through a proxy that adds a few headers, parsed
// into a table that indexes the common header names as workerd's does.
struct ApiHeadersRequest: public benchmark::Fixture {
  virtual ~ApiHeadersRequest() noexcept(true) {}

  void SetUp(benchmark::State& state) noexcept(true) override {
    fixture = kj::heap<TestFixture>();

    kj::HttpHeaderTable::Builder builder;
    for (auto name:
        {"Accept-Charset", "Accept-Encoding", "Accept-Language", "Accept-Ranges", "Accept",
          "Access-Control-Allow-Origin", "Age", "Allow", "Authorization", "Cache-Control",
          "Content-Disposition", "Content-Encoding", "Content-Language", "Content-Length",
          "Content-Location", "Content-Range", "Content-Type", "Cookie", "Date", "ETag", "Expect",
          "Expires", "From", "Host", "If-Match", "If-Modified-Since", "If-None-Match", "If-Range",
          "If-Unmodified-Since", "Last-Modified", "Link", "Location", "Max-Forwards",
          "Proxy-Authenticate", "Proxy-Authorization", "Range", "Referer", "Refresh", "Retry-After",
          "Server", "Set-Cookie", "Strict-Transport-Security", "Transfer-Encoding", "User-Agent",
          "Vary", "Via", "WWW-Authenticate", "CF-Ray", "CF-Cache-Status", "Pragma", "Origin"}) {
      builder.add(name);
    }
    table = builder.build();
    kjHeaders = kj::heap<kj::HttpHeaders>(*table);
    auto in = kj::heapString(
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,*/*;q=0.8\r\n"
        "Accept-Encoding: gzip, deflate, br, zstd\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Cache-Control: max-age=0\r\n"
        "Cookie: session=abc123def456; theme=dark; _ga=GA1.1.123456789.1700000000\r\n"
        "Priority: u=0, i\r\n"
        "Referer: https://example.com/start\r\n"
        "Sec-Ch-Ua: \"Chromium\";v=\"131\", \"Not_A Brand\";v=\"24\"\r\n"
        "Sec-Ch-Ua-Mobile: ?0\r\n"
        "Sec-Ch-Ua-Platform: \"macOS\"\r\n"
        "Sec-Fetch-Dest: document\r\n"
        "Sec-Fetch-Mode: navigate\r\n"
        "Sec-Fetch-Site: same-origin\r\n"
        "Sec-Fetch-User: ?1\r\n"
        "Upgrade-Insecure-Requests: 1\r\n"
        "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36\r\n"
        "CF-Connecting-IP: 203.0.113.7\r\n"
        "CF-IPCountry: US\r\n"
        "CF-Ray: 8f0c1d2e3a4b5c6d-SJC\r\n"
        "CF-Visitor: {\"scheme\":\"https\"}\r\n"
        "X-Forwarded-For: 203.0.113.7\r\n"
        "X-Forwarded-Proto: https\r\n"
        "\r\n");
    KJ_EXPECT(kjHeaders->tryParseRequest(in.asArray()).is<kj::HttpHeaders::Request>());
    original = kj::mv(in);
  }

  void TearDown(benchmark::State& state) noexcept(true) override {
    fixture = nullptr;
  }

  template <typename Func>
  void run(benchmark::State& state, Func&& func) {
    fixture->runInIoContext([&](const TestFixture::Environment& env) {
      for (auto _: state) {
        for (size_t i = 0; i < 1000; ++i) {
          auto headers =
              env.js.alloc<api::Headers>(env.js, *kjHeaders, api::Headers::Guard::REQUEST);
          func(env.js, *headers);
          benchmark::DoNotOptimize(headers);
        }
      }
    });
  }

  kj::Maybe<kj::String> original;
  kj::Own<TestFixture> fixture;
  kj::Own<kj::HttpHeaderTable> table;
  kj::Own<kj::HttpHeaders> kjHeaders;
};

// Built for a request whose headers JavaScript never reads.
BENCHMARK_F(ApiHeadersRequest, construct)(benchmark::State& state) {
  run(state, [](jsg::Lock&, api::Headers&) {});
}

// Two reads by name, as a Worker checking a header or two would.
BENCHMARK_F(ApiHeadersRequest, construct_get2)(benchmark::State& state) {
  run(state, [](jsg::Lock& js, api::Headers& headers) {
    benchmark::DoNotOptimize(headers.getPtr(js, "user-agent"_kj));
    benchmark::DoNotOptimize(headers.getPtr(js, "cf-connecting-ip"_kj));
  });
}

// Iterated, as Object.fromEntries(request.headers) does.
BENCHMARK_F(ApiHeadersRequest, construct_iterate)(benchmark::State& state) {
  run(state, [](jsg::Lock& js, api::Headers& headers) {
    benchmark::DoNotOptimize(headers.getDisplayedHeaders(js));
  });
}

// Copied back to a kj::HttpHeaders, as fetch(request) does.
BENCHMARK_F(ApiHeadersRequest, construct_copyToKj)(benchmark::State& state) {
  run(state, [this](jsg::Lock&, api::Headers& headers) {
    kj::HttpHeaders out(*table);
    headers.shallowCopyTo(out);
    benchmark::DoNotOptimize(out);
  });
}

// Changed and then copied to a kj::HttpHeaders, as a proxy that edits a header would.
BENCHMARK_F(ApiHeadersRequest, construct_set_copyToKj)(benchmark::State& state) {
  run(state, [this](jsg::Lock& js, api::Headers& headers) {
    headers.setUnguarded(js, kj::str("X-Added"), kj::str("1"));
    kj::HttpHeaders out(*table);
    headers.shallowCopyTo(out);
    benchmark::DoNotOptimize(out);
  });
}

}  // namespace
}  // namespace workerd
