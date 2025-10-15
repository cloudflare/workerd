// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

// Benchmark for TextEncoder.encode() and TextEncoder.encodeInto() methods.
// Tests performance across different character types (ASCII, one-byte UTF-8, two-byte UTF-8)
// and various string lengths (32, 256, 1024, 8192 characters) to measure UTF-8 encoding overhead.

namespace workerd {
namespace {

struct TextEncoderBenchmark: public benchmark::Fixture {
  virtual ~TextEncoderBenchmark() noexcept(true) {}

  void SetUp(benchmark::State& state) noexcept(true) override {
    TestFixture::SetupParams params = {.mainModuleSource = R"(
        const encoder = new TextEncoder();

        export default {
          async fetch(request) {
            const url = new URL(request.url);
            const len = parseInt(url.searchParams.get('len') || '256');
            const type = url.searchParams.get('type') || 'ascii';
            const op = url.searchParams.get('op') || 'encode';

            let base = '';
            switch (type) {
              case 'ascii':
                base = 'a';
                break;
              case 'one-byte':
                base = '\xff';
                break;
              case 'two-byte':
                base = 'ğ';
                break;
            }

            const input = base.repeat(len);

            let result;
            if (op === 'encode') {
              result = encoder.encode(input);
              return new Response(result.length.toString());
            } else if (op === 'encodeInto') {
              const buffer = new Uint8Array(len * 3); // enough space for any UTF-8 encoding
              result = encoder.encodeInto(input, buffer);
              return new Response(result.written.toString());
            }

            throw new Error('Invalid operation');
          },
        };
      )"_kj};
    fixture = kj::heap<TestFixture>(kj::mv(params));
  }

  void TearDown(benchmark::State& state) noexcept(true) override {
    fixture = nullptr;
  }

  kj::Own<TestFixture> fixture;
};

BENCHMARK_F(TextEncoderBenchmark, Encode_ASCII_32)(benchmark::State& state) {
  for (auto _: state) {
    benchmark::DoNotOptimize(fixture->runRequest(
        kj::HttpMethod::GET, "http://example.com?op=encode&type=ascii&len=32"_kj, ""_kj));
  }
}

BENCHMARK_F(TextEncoderBenchmark, Encode_ASCII_256)(benchmark::State& state) {
  for (auto _: state) {
    benchmark::DoNotOptimize(fixture->runRequest(
        kj::HttpMethod::GET, "http://example.com?op=encode&type=ascii&len=256"_kj, ""_kj));
  }
}

BENCHMARK_F(TextEncoderBenchmark, Encode_ASCII_1024)(benchmark::State& state) {
  for (auto _: state) {
    benchmark::DoNotOptimize(fixture->runRequest(
        kj::HttpMethod::GET, "http://example.com?op=encode&type=ascii&len=1024"_kj, ""_kj));
  }
}

BENCHMARK_F(TextEncoderBenchmark, Encode_ASCII_8192)(benchmark::State& state) {
  for (auto _: state) {
    benchmark::DoNotOptimize(fixture->runRequest(
        kj::HttpMethod::GET, "http://example.com?op=encode&type=ascii&len=8192"_kj, ""_kj));
  }
}

BENCHMARK_F(TextEncoderBenchmark, Encode_OneByte_256)(benchmark::State& state) {
  for (auto _: state) {
    benchmark::DoNotOptimize(fixture->runRequest(
        kj::HttpMethod::GET, "http://example.com?op=encode&type=one-byte&len=256"_kj, ""_kj));
  }
}

BENCHMARK_F(TextEncoderBenchmark, Encode_TwoByte_256)(benchmark::State& state) {
  for (auto _: state) {
    benchmark::DoNotOptimize(fixture->runRequest(
        kj::HttpMethod::GET, "http://example.com?op=encode&type=two-byte&len=256"_kj, ""_kj));
  }
}

BENCHMARK_F(TextEncoderBenchmark, EncodeInto_ASCII_256)(benchmark::State& state) {
  for (auto _: state) {
    benchmark::DoNotOptimize(fixture->runRequest(
        kj::HttpMethod::GET, "http://example.com?op=encodeInto&type=ascii&len=256"_kj, ""_kj));
  }
}

BENCHMARK_F(TextEncoderBenchmark, EncodeInto_OneByte_256)(benchmark::State& state) {
  for (auto _: state) {
    benchmark::DoNotOptimize(fixture->runRequest(
        kj::HttpMethod::GET, "http://example.com?op=encodeInto&type=one-byte&len=256"_kj, ""_kj));
  }
}

BENCHMARK_F(TextEncoderBenchmark, EncodeInto_TwoByte_256)(benchmark::State& state) {
  for (auto _: state) {
    benchmark::DoNotOptimize(fixture->runRequest(
        kj::HttpMethod::GET, "http://example.com?op=encodeInto&type=two-byte&len=256"_kj, ""_kj));
  }
}

}  // namespace
}  // namespace workerd
