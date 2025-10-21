// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/http.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

// A benchmark for Response object construction to identify performance bottlenecks.

namespace workerd {
namespace {

struct Response: public benchmark::Fixture {
  virtual ~Response() noexcept(true) {}

  void SetUp(benchmark::State& state) noexcept(true) override {
    fixture = kj::heap<TestFixture>();
  }

  void TearDown(benchmark::State& state) noexcept(true) override {
    fixture = nullptr;
  }

  kj::Own<TestFixture> fixture;
};

// Benchmark: Simple string body Response (most common case)
// Pattern: new Response("Hello World")
BENCHMARK_F(Response, simpleStringBody)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto _: state) {
      auto body = api::Body::Initializer(kj::str("Hello World"));
      benchmark::DoNotOptimize(api::Response::constructor(js, kj::mv(body), kj::none));
    }
  });
}

// Benchmark: Response with empty body (null)
// Pattern: new Response(null, {status: 404})
BENCHMARK_F(Response, nullBodyWithStatus)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto _: state) {
      api::Response::InitializerDict init;
      init.status = 404;
      benchmark::DoNotOptimize(api::Response::constructor(js, kj::none, kj::mv(init)));
    }
  });
}

// Benchmark: Response with headers
// Pattern: new Response("body", {headers: {"Content-Type": "text/html"}})
BENCHMARK_F(Response, bodyWithHeaders)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto _: state) {
      api::Response::InitializerDict init;
      jsg::Dict<jsg::ByteString, jsg::ByteString> headersDict;
      headersDict.fields = kj::heapArray<jsg::Dict<jsg::ByteString, jsg::ByteString>::Field>(1);
      headersDict.fields[0].name = jsg::ByteString(kj::str("Content-Type"));
      headersDict.fields[0].value = jsg::ByteString(kj::str("text/html"));
      init.headers = kj::mv(headersDict);

      auto body = api::Body::Initializer(kj::str("Hello World"));
      benchmark::DoNotOptimize(api::Response::constructor(js, kj::mv(body), kj::mv(init)));
    }
  });
}

// Benchmark: Response with ArrayBuffer body
// Pattern: new Response(arrayBuffer)
BENCHMARK_F(Response, arrayBufferBody)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto _: state) {
      auto bytes = kj::heapArray<byte>(11);
      memcpy(bytes.begin(), "Hello World", 11);
      auto body = api::Body::Initializer(kj::mv(bytes));
      benchmark::DoNotOptimize(api::Response::constructor(js, kj::mv(body), kj::none));
    }
  });
}

// Benchmark: Response.json()
// Pattern: Response.json({key: "value"})
BENCHMARK_F(Response, jsonResponse)(benchmark::State& state) {
  fixture->runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    for (auto _: state) {
      auto obj = js.obj();
      obj.set(js, js.str("key"_kj), js.str("value"_kj));
      benchmark::DoNotOptimize(api::Response::json_(js, obj, kj::none));
    }
  });
}

}  // namespace
}  // namespace workerd
