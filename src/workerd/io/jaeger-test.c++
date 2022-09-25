// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/jaeger.h>
#include <kj/test.h>
#include <capnp/message.h>
#include <workerd/io/jaeger-model.pb.h>

namespace workerd::tests {
namespace {

KJ_TEST("can read trace ID string format") {
  KJ_EXPECT(Jaeger::TraceId::fromGoString("z"_kj) == nullptr);

  KJ_EXPECT(Jaeger::TraceId::fromGoString("fedcba9876543210z"_kj) == nullptr);

  // Go parser supports non-(64 or 128) bit lengths -- unclear if anything cares.
  KJ_EXPECT(Jaeger::TraceId(0, 0) ==
      KJ_ASSERT_NONNULL(Jaeger::TraceId::fromGoString(""_kj)));
  KJ_EXPECT(Jaeger::TraceId(0x1, 0) ==
      KJ_ASSERT_NONNULL(Jaeger::TraceId::fromGoString("1"_kj)));

  KJ_EXPECT(Jaeger::TraceId(0xfedcba9876543210, 0) ==
      KJ_ASSERT_NONNULL(Jaeger::TraceId::fromGoString("fedcba9876543210"_kj)));
  KJ_EXPECT(Jaeger::TraceId(0xfedcba9876543210, 0) ==
      KJ_ASSERT_NONNULL(Jaeger::TraceId::fromGoString("FEDCBA9876543210"_kj)));

  KJ_EXPECT(Jaeger::TraceId(0xfedcba9876543210, 0x1) ==
      KJ_ASSERT_NONNULL(Jaeger::TraceId::fromGoString("01fedcba9876543210"_kj)));

  KJ_EXPECT(Jaeger::TraceId(0xfedcba9876543211, 0xfedcba9876543212) ==
      KJ_ASSERT_NONNULL(Jaeger::TraceId::fromGoString("fedcba9876543212fedcba9876543211"_kj)));

  KJ_EXPECT(Jaeger::TraceId::fromGoString("01fedcba9876543212fedcba9876543211"_kj) == nullptr);
}

KJ_TEST("can write trace ID string format") {
  KJ_EXPECT(Jaeger::TraceId(0x1, 0).toGoString() == "0000000000000001"_kj);
  KJ_EXPECT(Jaeger::TraceId(0xfedcba9876543210, 0).toGoString() == "fedcba9876543210"_kj);
  KJ_EXPECT(Jaeger::TraceId(0xfedcba9876543210, 0x1).toGoString() ==
      "0000000000000001fedcba9876543210"_kj);

  KJ_EXPECT(Jaeger::TraceId(0xfedcba9876543211, 0xfedcba9876543212).toGoString() ==
      "fedcba9876543212fedcba9876543211"_kj);
}

KJ_TEST("can read trace ID protobuf format") {
  KJ_EXPECT(Jaeger::TraceId::fromProtobuf(kj::heapArray("", 0).asBytes()) == nullptr);
  KJ_EXPECT(Jaeger::TraceId::fromProtobuf(kj::heapArray("z", 1).asBytes()) == nullptr);
  KJ_EXPECT(Jaeger::TraceId::fromProtobuf(kj::heapArray(
      "\xfe\xdc\xba\x98\x76\x54\x32\x12\xfe", 9).asBytes()) == nullptr);
  KJ_EXPECT(Jaeger::TraceId::fromProtobuf(kj::heapArray(
      "\xfe\xdc\xba\x98\x76\x54\x32\x12\xfe\xdc\xba\x98\x76\x54\x32\x11\x01", 17).asBytes())
      == nullptr);

  KJ_EXPECT(KJ_ASSERT_NONNULL(Jaeger::TraceId::fromProtobuf(kj::heapArray(
      "\xfe\xdc\xba\x98\x76\x54\x32\x12\xfe\xdc\xba\x98\x76\x54\x32\x11", 16).asBytes()))
      == Jaeger::TraceId(0xfedcba9876543211, 0xfedcba9876543212));
}

KJ_TEST("can write trace ID protobuf format") {
  KJ_EXPECT(Jaeger::TraceId(0, 0).toProtobuf() ==
      kj::heapArray("\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
                    16).asBytes());

  KJ_EXPECT(Jaeger::TraceId(0xfedcba9876543211, 0xfedcba9876543212).toProtobuf() ==
      kj::heapArray("\xfe\xdc\xba\x98\x76\x54\x32\x12\xfe\xdc\xba\x98\x76\x54\x32\x11",
                    16).asBytes());
}

KJ_TEST("can read span ID string format") {
  KJ_EXPECT(Jaeger::SpanId::fromGoString("z"_kj) == nullptr);
  KJ_EXPECT(Jaeger::SpanId::fromGoString("fedcba987654321z"_kj) == nullptr);

  // Go parser supports non-64 bit lengths -- unclear if anything cares.
  KJ_EXPECT(Jaeger::SpanId(0) ==
      KJ_ASSERT_NONNULL(Jaeger::SpanId::fromGoString(""_kj)));
  KJ_EXPECT(Jaeger::SpanId(0x1) ==
      KJ_ASSERT_NONNULL(Jaeger::SpanId::fromGoString("1"_kj)));

  KJ_EXPECT(Jaeger::SpanId(0xfedcba9876543210) ==
      KJ_ASSERT_NONNULL(Jaeger::SpanId::fromGoString("fedcba9876543210"_kj)));
  KJ_EXPECT(Jaeger::SpanId(0xfedcba9876543210) ==
      KJ_ASSERT_NONNULL(Jaeger::SpanId::fromGoString("FEDCBA9876543210"_kj)));

  KJ_EXPECT(Jaeger::SpanId::fromGoString("01fedcba9876543210"_kj) == nullptr);
}

KJ_TEST("can write span ID string format") {
  KJ_EXPECT(Jaeger::SpanId(0).toGoString() == "0000000000000000"_kj);
  KJ_EXPECT(Jaeger::SpanId(1).toGoString() == "0000000000000001"_kj);
  KJ_EXPECT(Jaeger::SpanId(0xfedcba9876543210).toGoString() == "fedcba9876543210"_kj);
}

KJ_TEST("can read span ID protobuf format") {
  KJ_EXPECT(Jaeger::SpanId::fromProtobuf(kj::heapArray("", 0).asBytes()) == nullptr);
  KJ_EXPECT(Jaeger::SpanId::fromProtobuf(kj::heapArray("z", 1).asBytes()) == nullptr);
  KJ_EXPECT(Jaeger::SpanId::fromProtobuf(kj::heapArray(
      "\xfe\xdc\xba\x98\x76\x54\x32\x12\xfe", 9).asBytes()) == nullptr);

  KJ_EXPECT(KJ_ASSERT_NONNULL(Jaeger::SpanId::fromProtobuf(kj::heapArray(
      "\xfe\xdc\xba\x98\x76\x54\x32\x10", 8).asBytes()))
      == Jaeger::SpanId(0xfedcba9876543210));
}

KJ_TEST("can write span ID protobuf format") {
  KJ_EXPECT(Jaeger::SpanId(0).toProtobuf() ==
      kj::heapArray("\x00\x00\x00\x00\x00\x00\x00\x00", 8).asBytes());

  KJ_EXPECT(Jaeger::SpanId(0xfedcba9876543210).toProtobuf() ==
      kj::heapArray("\xfe\xdc\xba\x98\x76\x54\x32\x10", 8).asBytes());
}

KJ_TEST("can parse span header") {
  KJ_EXPECT(Jaeger::SpanContext::fromHeader("c3adb70e6fce1825:c6e1011ff2ea0fb3:d11b288de039af9e"_kj) == nullptr);
  KJ_EXPECT(Jaeger::SpanContext::fromHeader("c3adb70e6fce1825:c6e1011ff2ea0fb3:d11b288de039af9e:0:0"_kj) == nullptr);
  KJ_EXPECT(Jaeger::SpanContext::fromHeader("x3adb70e6fce1825:c6e1011ff2ea0fb3:d11b288de039af9e:0"_kj) == nullptr);
  KJ_EXPECT(Jaeger::SpanContext::fromHeader("c3adb70e6fce1825:x6e1011ff2ea0fb3:d11b288de039af9e:0"_kj) == nullptr);
  KJ_EXPECT(Jaeger::SpanContext::fromHeader("c3adb70e6fce1825:c6e1011ff2ea0fb3:x11b288de039af9e:0"_kj) == nullptr);
  KJ_EXPECT(Jaeger::SpanContext::fromHeader("c3adb70e6fce1825:c6e1011ff2ea0fb3:d11b288de039af9e:x"_kj) == nullptr);

  KJ_EXPECT(KJ_ASSERT_NONNULL(Jaeger::SpanContext::fromHeader(
      "c3adb70e6fce1825:c6e1011ff2ea0fb3:d11b288de039af9e:0"_kj)) ==
      Jaeger::SpanContext(Jaeger::TraceId(0xc3adb70e6fce1825, 0),
                          Jaeger::SpanId(0xc6e1011ff2ea0fb3),
                          Jaeger::SpanId(0xd11b288de039af9e),
                          0));
}

KJ_TEST("can write span header") {
  KJ_EXPECT("c3adb70e6fce1825:c6e1011ff2ea0fb3:d11b288de039af9e:0"_kj ==
      Jaeger::SpanContext(Jaeger::TraceId(0xc3adb70e6fce1825, 0),
                          Jaeger::SpanId(0xc6e1011ff2ea0fb3),
                          Jaeger::SpanId(0xd11b288de039af9e),
                          0).toHeader());
}

static constexpr auto GOLDEN_SPAN_DATA = R"(trace_id: "\000\000\000\000\000\000\000\000\303\255\267\016o\316\030%"
span_id: "\306\341\001\037\362\352\017\263"
operation_name: "test_span"
references {
  trace_id: "\000\000\000\000\000\000\000\000\303\255\267\016o\316\030%"
  span_id: "\321\033(\215\3409\257\236"
}
start_time {
  seconds: 1
}
duration {
}
tags {
  key: "default tag key"
  v_str: "default tag value"
}
tags {
  key: "bool_tag"
  v_type: BOOL
  v_bool: true
}
tags {
  key: "int64_tag"
  v_type: INT64
  v_int64: 123
}
tags {
  key: "float64_tag"
  v_type: FLOAT64
  v_float64: 3.14159
}
tags {
  key: "string_tag"
  v_str: "string tag value"
}
logs {
  timestamp {
    seconds: 123
  }
  fields {
    key: "log_key"
    v_type: FLOAT64
    v_float64: 3.14159
  }
}
logs {
  timestamp {
    seconds: 456
  }
  fields {
    key: "another_log_key"
    v_str: "can write more than one log"
  }
}
process {
  service_name: "foo-service"
  tags {
    key: "hostname"
    v_str: "123m5"
  }
  tags {
    key: "coloId"
    v_str: "123"
  }
  tags {
    key: "colo_name"
    v_str: "abc01"
  }
  tags {
    key: "cordon"
    v_str: "paid"
  }
}
)"_kj;

KJ_TEST("can write span data") {
  auto spanContext = KJ_ASSERT_NONNULL(Jaeger::SpanContext::fromHeader(
      "c3adb70e6fce1825:c6e1011ff2ea0fb3:d11b288de039af9e:0"_kj));
  auto spanData = Jaeger::SpanData(
      spanContext,
      "test_span"_kj,
      kj::origin<kj::Date>() + 1 * kj::SECONDS);

  spanData.tags.insert("bool_tag"_kj, true);
  spanData.tags.insert("int64_tag"_kj, 123l);
  spanData.tags.insert("float64_tag"_kj, 3.14159);
  spanData.tags.insert("string_tag"_kj, kj::str("string tag value"));

  spanData.logs.add(Jaeger::SpanData::Log {
    .timestamp = kj::UNIX_EPOCH + 123 * kj::SECONDS,
    .tag = {
      .key = "log_key"_kj,
      .value = 3.14159,
    }
  });
  spanData.logs.add(Jaeger::SpanData::Log {
    .timestamp = kj::UNIX_EPOCH + 456 * kj::SECONDS,
    .tag = {
      .key = "another_log_key"_kj,
      .value = kj::str("can write more than one log"),
    }
  });

  using Tag = Jaeger::SpanData::Tag;
  static Tag processTags[] = {
    Tag { .key = "hostname"_kj,  .value = kj::str("123m5") },
    // TODO(cleanup): Use int64_t for `coloId`.
    Tag { .key = "coloId"_kj,    .value = kj::str(123) },
    // Snake-case used for colo_name to match tracefwdr's convention.
    Tag { .key = "colo_name"_kj, .value = kj::str("abc01") },
    Tag { .key = "cordon"_kj,    .value = kj::str("paid") },
  };
  kj::Vector<Jaeger::SpanData::Tag> defaultTags;
  defaultTags.add(Jaeger::SpanData::Tag {
    .key = "default tag key"_kj,
    .value = kj::str("default tag value")
  });
  auto protobuf = spanData.toProtobuf(processTags, defaultTags, "foo-service");

  jaeger::api_v2::Span span;
  KJ_REQUIRE(span.ParseFromArray(protobuf.begin(), protobuf.size()), protobuf);
  std::string messageStr = span.DebugString();
  auto message = kj::StringPtr(messageStr.c_str(), messageStr.size());

  KJ_EXPECT(GOLDEN_SPAN_DATA == message);
}

}  // anonymous namespace
} // namespace workerd::tests
