// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/trace.h>

#include <kj/test.h>

namespace workerd::tracing {
namespace {

KJ_TEST("can read trace ID string format") {
  KJ_EXPECT(TraceId::fromGoString("z"_kj) == kj::none);

  KJ_EXPECT(TraceId::fromGoString("fedcba9876543210z"_kj) == kj::none);

  // Go parser supports non-(64 or 128) bit lengths -- unclear if anything cares.
  KJ_EXPECT(TraceId(0, 0) == KJ_ASSERT_NONNULL(TraceId::fromGoString(""_kj)));
  KJ_EXPECT(TraceId(0x1, 0) == KJ_ASSERT_NONNULL(TraceId::fromGoString("1"_kj)));

  KJ_EXPECT(TraceId(0xfedcba9876543210, 0) ==
      KJ_ASSERT_NONNULL(TraceId::fromGoString("fedcba9876543210"_kj)));
  KJ_EXPECT(TraceId(0xfedcba9876543210, 0) ==
      KJ_ASSERT_NONNULL(TraceId::fromGoString("FEDCBA9876543210"_kj)));

  KJ_EXPECT(TraceId(0xfedcba9876543210, 0x1) ==
      KJ_ASSERT_NONNULL(TraceId::fromGoString("01fedcba9876543210"_kj)));

  KJ_EXPECT(TraceId(0xfedcba9876543211, 0xfedcba9876543212) ==
      KJ_ASSERT_NONNULL(TraceId::fromGoString("fedcba9876543212fedcba9876543211"_kj)));

  KJ_EXPECT(TraceId::fromGoString("01fedcba9876543212fedcba9876543211"_kj) == kj::none);
}

KJ_TEST("can write trace ID string format") {
  KJ_EXPECT(TraceId(0x1, 0).toGoString() == "0000000000000001"_kj);
  KJ_EXPECT(TraceId(0xfedcba9876543210, 0).toGoString() == "fedcba9876543210"_kj);
  KJ_EXPECT(TraceId(0xfedcba9876543210, 0x1).toGoString() == "0000000000000001fedcba9876543210"_kj);

  KJ_EXPECT(TraceId(0xfedcba9876543211, 0xfedcba9876543212).toGoString() ==
      "fedcba9876543212fedcba9876543211"_kj);
}

KJ_TEST("can read trace ID protobuf format") {
  KJ_EXPECT(TraceId::fromProtobuf(""_kjb) == kj::none);
  KJ_EXPECT(TraceId::fromProtobuf("z"_kjb) == kj::none);
  KJ_EXPECT(TraceId::fromProtobuf("\xfe\xdc\xba\x98\x76\x54\x32\x12\xfe"_kjb) == kj::none);
  KJ_EXPECT(
      TraceId::fromProtobuf(
          "\xfe\xdc\xba\x98\x76\x54\x32\x12\xfe\xdc\xba\x98\x76\x54\x32\x11\x01"_kjb) == kj::none);

  KJ_EXPECT(KJ_ASSERT_NONNULL(TraceId::fromProtobuf(
                "\xfe\xdc\xba\x98\x76\x54\x32\x12\xfe\xdc\xba\x98\x76\x54\x32\x11"_kjb)) ==
      TraceId(0xfedcba9876543211, 0xfedcba9876543212));
}

KJ_TEST("can write trace ID protobuf format") {
  KJ_EXPECT(TraceId(0, 0).toProtobuf() ==
      "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"_kjb);

  KJ_EXPECT(TraceId(0xfedcba9876543211, 0xfedcba9876543212).toProtobuf() ==
      "\xfe\xdc\xba\x98\x76\x54\x32\x12\xfe\xdc\xba\x98\x76\x54\x32\x11"_kjb);
}

}  // namespace
}  // namespace workerd::tracing
