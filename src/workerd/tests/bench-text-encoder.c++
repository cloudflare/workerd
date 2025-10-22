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

struct TextEncoder: public benchmark::Fixture {
  virtual ~TextEncoder() noexcept(true) {}

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
              for (let i = 0; i < 100_000; i++) {
                result = encoder.encode(input);
              }
              return new Response(result.length.toString());
            } else if (op === 'encodeInto') {
              const buffer = new Uint8Array(len * 3); // enough space for any UTF-8 encoding
              for (let i = 0; i < 100_000; i++) {
                result = encoder.encodeInto(input, buffer);
              }
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

// Parameterized benchmark to avoid duplication
// Args format: (operation, type, length)
// operation: 0=encode, 1=encodeInto
// type: 0=ascii, 1=one-byte, 2=two-byte
BENCHMARK_DEFINE_F(TextEncoder, Parameterized)(benchmark::State& state) {
  const char* op = state.range(0) == 0 ? "encode" : "encodeInto";
  const char* type;
  switch (state.range(1)) {
    case 0:
      type = "ascii";
      break;
    case 1:
      type = "one-byte";
      break;
    case 2:
      type = "two-byte";
      break;
    default:
      type = "ascii";
      break;
  }
  int64_t len = state.range(2);

  auto url = kj::str("http://example.com?op=", op, "&type=", type, "&len=", len);

  for (auto _: state) {
    benchmark::DoNotOptimize(fixture->runRequest(kj::HttpMethod::GET, url, ""_kj));
  }
}

#define TEXT_ENCODER_BENCH(op_name, op_val, type_name, type_val, len)                              \
  BENCHMARK_REGISTER_F(TextEncoder, Parameterized)                                                 \
      ->Args({op_val, type_val, len})                                                              \
      ->Name(#op_name "_" #type_name "_" #len)

// Note: Google Benchmark will append the arg values to the name (e.g., "Encode_ASCII_32/0/0/32")
// where the trailing numbers are the actual argument values passed via ->Args():
//   /0/0/32 = operation (0=encode, 1=encodeInto) / type (0=ascii, 1=one-byte, 2=two-byte) / length
TEXT_ENCODER_BENCH(Encode, 0, ASCII, 0, 32);
TEXT_ENCODER_BENCH(Encode, 0, ASCII, 0, 256);
TEXT_ENCODER_BENCH(Encode, 0, ASCII, 0, 1024);
TEXT_ENCODER_BENCH(Encode, 0, ASCII, 0, 8192);
TEXT_ENCODER_BENCH(Encode, 0, OneByte, 1, 256);
TEXT_ENCODER_BENCH(Encode, 0, OneByte, 1, 1024);
TEXT_ENCODER_BENCH(Encode, 0, OneByte, 1, 8192);
TEXT_ENCODER_BENCH(Encode, 0, TwoByte, 2, 256);
TEXT_ENCODER_BENCH(Encode, 0, TwoByte, 2, 1024);
TEXT_ENCODER_BENCH(Encode, 0, TwoByte, 2, 8192);
TEXT_ENCODER_BENCH(EncodeInto, 1, ASCII, 0, 256);
TEXT_ENCODER_BENCH(EncodeInto, 1, ASCII, 0, 1024);
TEXT_ENCODER_BENCH(EncodeInto, 1, ASCII, 0, 8192);
TEXT_ENCODER_BENCH(EncodeInto, 1, OneByte, 1, 256);
TEXT_ENCODER_BENCH(EncodeInto, 1, OneByte, 1, 1024);
TEXT_ENCODER_BENCH(EncodeInto, 1, OneByte, 1, 8192);
TEXT_ENCODER_BENCH(EncodeInto, 1, TwoByte, 2, 256);
TEXT_ENCODER_BENCH(EncodeInto, 1, TwoByte, 2, 1024);
TEXT_ENCODER_BENCH(EncodeInto, 1, TwoByte, 2, 8192);

}  // namespace
}  // namespace workerd
