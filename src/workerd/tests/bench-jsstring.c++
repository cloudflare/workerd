// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

// Benchmark for JsString.utf8Length() method across different string types:
// latin1/utf16, flat/non-flat, and various sizes.

namespace workerd {
namespace {

template <size_t N>
jsg::JsString createLatin1String(jsg::Lock& js) {
  kj::FixedArray<char, N> vec;
  vec.fill('a');
  if (N > 1) {
    vec[N / 2] = static_cast<char>(0xC0);
  }
  return js.str(vec.asPtr());
}

template <size_t N>
jsg::JsString createUtf16String(jsg::Lock& js) {
  kj::FixedArray<uint16_t, N> vec;
  vec.fill(0x1F600 & 0xFFFF);
  return js.str(vec.asPtr());
}

template <size_t N>
jsg::JsString createInvalidUtf16String(jsg::Lock& js) {
  kj::FixedArray<uint16_t, N> vec;
  vec.fill(0x1F600 & 0xFFFF);
  if (N > 1) {
    vec[N / 2] = 0xD800;
  }
  return js.str(vec.asPtr());
}

// Benchmarks utf8Length on 32-char latin1 flat strings
static void JsString_Utf8Length_Latin1_Flat_32(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto str = createLatin1String<32>(js);

    for (auto _: state) {
      KJ_ASSERT(str.isFlat());
      benchmark::DoNotOptimize(str.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 256-char latin1 flat strings
static void JsString_Utf8Length_Latin1_Flat_256(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto str = createLatin1String<256>(js);

    for (auto _: state) {
      KJ_ASSERT(str.isFlat());
      benchmark::DoNotOptimize(str.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 1024-char latin1 flat strings
static void JsString_Utf8Length_Latin1_Flat_1024(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto str = createLatin1String<1024>(js);

    for (auto _: state) {
      KJ_ASSERT(str.isFlat());
      benchmark::DoNotOptimize(str.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 8192-char latin1 flat strings
static void JsString_Utf8Length_Latin1_Flat_8192(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto str = createLatin1String<8192>(js);

    for (auto _: state) {
      KJ_ASSERT(str.isFlat());
      benchmark::DoNotOptimize(str.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 256-char utf16 flat strings
static void JsString_Utf8Length_Utf16_Flat_256(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto str = createUtf16String<256>(js);

    for (auto _: state) {
      KJ_ASSERT(str.isFlat());
      benchmark::DoNotOptimize(str.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 1024-char utf16 flat strings
static void JsString_Utf8Length_Utf16_Flat_1024(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto str = createUtf16String<1024>(js);

    for (auto _: state) {
      KJ_ASSERT(str.isFlat());
      benchmark::DoNotOptimize(str.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 8192-char utf16 flat strings
static void JsString_Utf8Length_Utf16_Flat_8192(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto str = createUtf16String<8192>(js);

    for (auto _: state) {
      KJ_ASSERT(str.isFlat());
      benchmark::DoNotOptimize(str.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 256-char utf16 flat strings with invalid UTF-16
static void JsString_Utf8Length_Utf16_Invalid_Flat_256(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto str = createInvalidUtf16String<256>(js);

    for (auto _: state) {
      KJ_ASSERT(str.isFlat());
      benchmark::DoNotOptimize(str.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 1024-char utf16 flat strings with invalid UTF-16
static void JsString_Utf8Length_Utf16_Invalid_Flat_1024(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto str = createInvalidUtf16String<1024>(js);

    for (auto _: state) {
      KJ_ASSERT(str.isFlat());
      benchmark::DoNotOptimize(str.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 8192-char utf16 flat strings with invalid UTF-16
static void JsString_Utf8Length_Utf16_Invalid_Flat_8192(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto str = createInvalidUtf16String<8192>(js);

    for (auto _: state) {
      KJ_ASSERT(str.isFlat());
      benchmark::DoNotOptimize(str.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 256-char non-flat latin1 strings (deep tree, 8 pieces)
static void JsString_Utf8Length_Latin1_NonFlat_256(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto piece = createLatin1String<32>(js);

    for (auto _: state) {
      // Build deep tree by concatenating 8 pieces (depth ~7-8)
      auto result = piece;
      for (int i = 1; i < 8; i++) {
        result = jsg::JsString::concat(js, result, piece);
      }
      KJ_ASSERT(!result.isFlat());
      benchmark::DoNotOptimize(result.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 1024-char non-flat latin1 strings (deep tree, 16 pieces)
static void JsString_Utf8Length_Latin1_NonFlat_1024(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto piece = createLatin1String<64>(js);

    for (auto _: state) {
      // Build deep tree by concatenating 16 pieces (depth ~15-16)
      auto result = piece;
      for (int i = 1; i < 16; i++) {
        result = jsg::JsString::concat(js, result, piece);
      }
      KJ_ASSERT(!result.isFlat());
      benchmark::DoNotOptimize(result.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 8192-char non-flat latin1 strings (deep tree, 32 pieces)
static void JsString_Utf8Length_Latin1_NonFlat_8192(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto piece = createLatin1String<256>(js);

    for (auto _: state) {
      // Build deep tree by concatenating 32 pieces (depth ~31-32)
      auto result = piece;
      for (int i = 1; i < 32; i++) {
        result = jsg::JsString::concat(js, result, piece);
      }
      KJ_ASSERT(!result.isFlat());
      benchmark::DoNotOptimize(result.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 256-char non-flat utf16 strings (deep tree, 8 pieces)
static void JsString_Utf8Length_Utf16_NonFlat_256(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto piece = createUtf16String<32>(js);

    for (auto _: state) {
      // Build deep tree by concatenating 8 pieces (depth ~7-8)
      auto result = piece;
      for (int i = 1; i < 8; i++) {
        result = jsg::JsString::concat(js, result, piece);
      }
      KJ_ASSERT(!result.isFlat());
      benchmark::DoNotOptimize(result.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 1024-char non-flat utf16 strings (deep tree, 16 pieces)
static void JsString_Utf8Length_Utf16_NonFlat_1024(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto piece = createUtf16String<64>(js);

    for (auto _: state) {
      // Build deep tree by concatenating 16 pieces (depth ~15-16)
      auto result = piece;
      for (int i = 1; i < 16; i++) {
        result = jsg::JsString::concat(js, result, piece);
      }
      KJ_ASSERT(!result.isFlat());
      benchmark::DoNotOptimize(result.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 8192-char non-flat utf16 strings (deep tree, 32 pieces)
static void JsString_Utf8Length_Utf16_NonFlat_8192(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto piece = createUtf16String<256>(js);

    for (auto _: state) {
      // Build deep tree by concatenating 32 pieces (depth ~31-32)
      auto result = piece;
      for (int i = 1; i < 32; i++) {
        result = jsg::JsString::concat(js, result, piece);
      }
      KJ_ASSERT(!result.isFlat());
      benchmark::DoNotOptimize(result.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 256-char non-flat utf16 strings with invalid UTF-16 (deep tree, 8 pieces)
static void JsString_Utf8Length_Utf16_Invalid_NonFlat_256(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto piece = createInvalidUtf16String<32>(js);

    for (auto _: state) {
      // Build deep tree by concatenating 8 pieces (depth ~7-8)
      auto result = piece;
      for (int i = 1; i < 8; i++) {
        result = jsg::JsString::concat(js, result, piece);
      }
      KJ_ASSERT(!result.isFlat());
      benchmark::DoNotOptimize(result.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 1024-char non-flat utf16 strings with invalid UTF-16 (deep tree, 16 pieces)
static void JsString_Utf8Length_Utf16_Invalid_NonFlat_1024(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto piece = createInvalidUtf16String<64>(js);

    for (auto _: state) {
      // Build deep tree by concatenating 16 pieces (depth ~15-16)
      auto result = piece;
      for (int i = 1; i < 16; i++) {
        result = jsg::JsString::concat(js, result, piece);
      }
      KJ_ASSERT(!result.isFlat());
      benchmark::DoNotOptimize(result.utf8Length(js));
    }
  });
}

// Benchmarks utf8Length on 8192-char non-flat utf16 strings with invalid UTF-16 (deep tree, 32 pieces)
static void JsString_Utf8Length_Utf16_Invalid_NonFlat_8192(benchmark::State& state) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto piece = createInvalidUtf16String<256>(js);

    for (auto _: state) {
      // Build deep tree by concatenating 32 pieces (depth ~31-32)
      auto result = piece;
      for (int i = 1; i < 32; i++) {
        result = jsg::JsString::concat(js, result, piece);
      }
      KJ_ASSERT(!result.isFlat());
      benchmark::DoNotOptimize(result.utf8Length(js));
    }
  });
}

WD_BENCHMARK(JsString_Utf8Length_Latin1_Flat_32);
WD_BENCHMARK(JsString_Utf8Length_Latin1_Flat_256);
WD_BENCHMARK(JsString_Utf8Length_Latin1_Flat_1024);
WD_BENCHMARK(JsString_Utf8Length_Latin1_Flat_8192);
WD_BENCHMARK(JsString_Utf8Length_Utf16_Flat_256);
WD_BENCHMARK(JsString_Utf8Length_Utf16_Flat_1024);
WD_BENCHMARK(JsString_Utf8Length_Utf16_Flat_8192);
WD_BENCHMARK(JsString_Utf8Length_Utf16_Invalid_Flat_256);
WD_BENCHMARK(JsString_Utf8Length_Utf16_Invalid_Flat_1024);
WD_BENCHMARK(JsString_Utf8Length_Utf16_Invalid_Flat_8192);
WD_BENCHMARK(JsString_Utf8Length_Latin1_NonFlat_256);
WD_BENCHMARK(JsString_Utf8Length_Latin1_NonFlat_1024);
WD_BENCHMARK(JsString_Utf8Length_Latin1_NonFlat_8192);
WD_BENCHMARK(JsString_Utf8Length_Utf16_NonFlat_256);
WD_BENCHMARK(JsString_Utf8Length_Utf16_NonFlat_1024);
WD_BENCHMARK(JsString_Utf8Length_Utf16_NonFlat_8192);
WD_BENCHMARK(JsString_Utf8Length_Utf16_Invalid_NonFlat_256);
WD_BENCHMARK(JsString_Utf8Length_Utf16_Invalid_NonFlat_1024);
WD_BENCHMARK(JsString_Utf8Length_Utf16_Invalid_NonFlat_8192);

}  // namespace
}  // namespace workerd
