// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Benchmark: v8::ValueSerializer / ValueDeserializer round-trip cost.
//
// Measures the CPU floor for any continuation-style suspend/resume
// scheme that snapshots live JS state across an I/O boundary.
//
// Five workload size classes (XS ... XL) reflecting realistic agent
// state. Each iteration rebuilds the workload with a fresh seed so
// V8's inline caches don't go hot — the production case is every
// continuation holds different state.
//
// Related issue: cloudflare/workerd#6595

#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

#include <v8-container.h>
#include <v8-context.h>
#include <v8-exception.h>
#include <v8-function.h>
#include <v8-isolate.h>
#include <v8-object.h>
#include <v8-primitive.h>
#include <v8-script.h>
#include <v8-value-serializer.h>

#include <kj/string.h>

#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

namespace workerd {
namespace {

enum class SizeClass { XS, S, M, L, XL };

kj::StringPtr sizeClassName(SizeClass s) {
  switch (s) {
    case SizeClass::XS: return "XS"_kj;
    case SizeClass::S:  return "S"_kj;
    case SizeClass::M:  return "M"_kj;
    case SizeClass::L:  return "L"_kj;
    case SizeClass::XL: return "XL"_kj;
  }
  KJ_UNREACHABLE;
}

class NullDelegate: public v8::ValueSerializer::Delegate {
 public:
  void ThrowDataCloneError(v8::Local<v8::String> message) override {
    v8::Isolate::GetCurrent()->ThrowException(v8::Exception::Error(message));
  }
};

std::string makeText(size_t approxBytes, uint32_t seed) {
  std::mt19937 rng(seed);
  std::string s;
  s.reserve(approxBytes);
  const char* alphabet = "abcdefghijklmnopqrstuvwxyz ";
  while (s.size() < approxBytes) {
    s.push_back(alphabet[rng() % 27]);
  }
  return s;
}

v8::Local<v8::String> v8Str(v8::Isolate* iso, const std::string& s) {
  return v8::String::NewFromUtf8(
      iso, s.c_str(), v8::NewStringType::kNormal, static_cast<int>(s.size()))
      .ToLocalChecked();
}

v8::Local<v8::String> v8Str(v8::Isolate* iso, const char* s) {
  return v8::String::NewFromUtf8(iso, s, v8::NewStringType::kNormal).ToLocalChecked();
}

void setField(v8::Isolate* iso,
              v8::Local<v8::Context> ctx,
              v8::Local<v8::Object> obj,
              const char* k,
              v8::Local<v8::Value> v) {
  obj->Set(ctx, v8Str(iso, k), v).Check();
}

v8::Local<v8::Object> buildWorkload(
    v8::Isolate* iso, v8::Local<v8::Context> ctx, SizeClass s, uint32_t seed) {
  auto o = v8::Object::New(iso);
  std::mt19937 rng(seed);

  switch (s) {
    case SizeClass::XS:
      setField(iso, ctx, o, "n", v8::Number::New(iso, 42.5 + (rng() % 1000)));
      setField(iso, ctx, o, "s", v8Str(iso, makeText(8, seed + 1)));
      return o;
    case SizeClass::S: {
      setField(iso, ctx, o, "url",
          v8Str(iso, "https://api.example.com/v1/agents/" + makeText(16, seed + 2)));
      setField(iso, ctx, o, "method", v8Str(iso, (seed % 2) ? "POST" : "GET"));
      auto h = v8::Object::New(iso);
      setField(iso, ctx, h, "content-type", v8Str(iso, "application/json"));
      setField(iso, ctx, h, "user-agent", v8Str(iso, "workerd/bench"));
      setField(iso, ctx, h, "accept", v8Str(iso, "*/*"));
      setField(iso, ctx, o, "headers", h);
      return o;
    }
    case SizeClass::M: {
      setField(iso, ctx, o, "url",
          v8Str(iso, "https://api.example.com/v1/agents/" + makeText(16, seed + 3)));
      setField(iso, ctx, o, "method", v8Str(iso, "POST"));
      auto cache = v8::Object::New(iso);
      setField(iso, ctx, cache, "etag", v8Str(iso, "\"" + makeText(11, seed + 4) + "\""));
      setField(iso, ctx, cache, "body", v8Str(iso, makeText(2048, seed + 5)));
      setField(iso, ctx, o, "cache", cache);
      setField(iso, ctx, o, "body", v8Str(iso, makeText(2048, seed + 6)));
      return o;
    }
    case SizeClass::L: {
      auto m = buildWorkload(iso, ctx, SizeClass::M, seed);
      auto vec = v8::Array::New(iso, 1024);
      std::mt19937 vrng(seed + 100);
      for (uint32_t i = 0; i < 1024; ++i) {
        double x = static_cast<double>(vrng()) / 4294967296.0 * 2.0 - 1.0;
        vec->Set(ctx, i, v8::Number::New(iso, x)).Check();
      }
      setField(iso, ctx, m, "embedding", vec);
      setField(iso, ctx, m, "doc", v8Str(iso, makeText(40 * 1024, seed + 200)));
      return m;
    }
    case SizeClass::XL: {
      auto m = buildWorkload(iso, ctx, SizeClass::M, seed);
      auto chat = v8::Array::New(iso, 30);
      for (uint32_t i = 0; i < 30; ++i) {
        auto msg = v8::Object::New(iso);
        setField(iso, ctx, msg, "role", v8Str(iso, (i % 2 == 0) ? "user" : "assistant"));
        setField(iso, ctx, msg, "content", v8Str(iso, makeText(15 * 1024, seed + 1000 + i)));
        chat->Set(ctx, i, msg).Check();
      }
      setField(iso, ctx, m, "chat", chat);
      return m;
    }
  }
  KJ_UNREACHABLE;
}

struct ValueSerializerBenchmark: public benchmark::Fixture {
  virtual ~ValueSerializerBenchmark() noexcept(true) {}

  void SetUp(benchmark::State& state) noexcept(true) override {
    fixture = kj::heap<TestFixture>();
    iterations = 0;
  }
  void TearDown(benchmark::State& state) noexcept(true) override { fixture = nullptr; }

  kj::Own<TestFixture> fixture;
  uint32_t iterations = 0;

  void runRoundTrip(benchmark::State& state, SizeClass sc) {
    fixture->runInIoContext([&](const auto& env) -> kj::Promise<void> {
      v8::Isolate* iso = env.isolate;
      v8::HandleScope outer(iso);
      v8::Local<v8::Context> ctx = iso->GetCurrentContext();

      // Warmup: 200 fresh-shape rebuilds + round-trips.
      for (int i = 0; i < 200; ++i) {
        v8::HandleScope hs(iso);
        auto obj = buildWorkload(iso, ctx, sc, 10000 + i);
        NullDelegate d;
        v8::ValueSerializer ser(iso, &d);
        ser.WriteHeader();
        ser.WriteValue(ctx, obj).Check();
        auto buf = ser.Release();
        v8::ValueDeserializer des(iso, buf.first, buf.second);
        des.ReadHeader(ctx).Check();
        (void)des.ReadValue(ctx);
        std::free(buf.first);
      }

      for (auto _: state) {
        v8::HandleScope hs(iso);
        ++iterations;
        auto obj = buildWorkload(iso, ctx, sc, iterations);

        NullDelegate d;
        v8::ValueSerializer ser(iso, &d);
        ser.WriteHeader();
        ser.WriteValue(ctx, obj).Check();
        auto buf = ser.Release();

        v8::ValueDeserializer des(iso, buf.first, buf.second);
        des.ReadHeader(ctx).Check();
        auto result = des.ReadValue(ctx);
        benchmark::DoNotOptimize(result);
        std::free(buf.first);
      }

      return kj::READY_NOW;
    });
  }
};

BENCHMARK_F(ValueSerializerBenchmark, XS)(benchmark::State& state) {
  runRoundTrip(state, SizeClass::XS);
}
BENCHMARK_F(ValueSerializerBenchmark, S)(benchmark::State& state) {
  runRoundTrip(state, SizeClass::S);
}
BENCHMARK_F(ValueSerializerBenchmark, M)(benchmark::State& state) {
  runRoundTrip(state, SizeClass::M);
}
BENCHMARK_F(ValueSerializerBenchmark, L)(benchmark::State& state) {
  runRoundTrip(state, SizeClass::L);
}
BENCHMARK_F(ValueSerializerBenchmark, XL)(benchmark::State& state) {
  runRoundTrip(state, SizeClass::XL);
}

}  // namespace
}  // namespace workerd
