// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/tests/bench-tools.h>

#include <kj/compat/http.h>

namespace workerd {
namespace {

struct KjHeaders: public benchmark::Fixture {
  virtual ~KjHeaders() noexcept(true) {}

  void SetUp(benchmark::State& state) noexcept(true) override {
    kj::HttpHeaderTable::Builder builder;
    builder.add("Host");
    builder.add("Accept");
    builder.add("Content-Type");
    builder.add("Last-Modified");
    table = builder.build();
  }

  kj::Own<kj::HttpHeaderTable> table;
};

BENCHMARK_F(KjHeaders, Parse)(benchmark::State& state) {
  for (auto _: state) {
    kj::HttpHeaders headers(*table);

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
    KJ_EXPECT(headers.tryParseRequest(in.asArray()).is<kj::HttpHeaders::Request>());
  }
}

}  // namespace
}  // namespace workerd
