// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/trace.h>
#include <workerd/util/thread-scopes.h>

#include <capnp/message.h>
#include <kj/compat/http.h>
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

KJ_TEST("InvocationSpanContext") {

  class FakeEntropySource final: public kj::EntropySource {
   public:
    void generate(kj::ArrayPtr<byte> buffer) override {
      // Write the uint64_t value to the buffer
      buffer[0] = counter & 0xff;
      buffer[1] = (counter >> 8) & 0xff;
      buffer[2] = (counter >> 16) & 0xff;
      buffer[3] = (counter >> 24) & 0xff;
      buffer[4] = (counter >> 32) & 0xff;
      buffer[5] = (counter >> 40) & 0xff;
      buffer[6] = (counter >> 48) & 0xff;
      buffer[7] = (counter >> 56) & 0xff;
      counter++;
    }

   private:
    uint64_t counter = 0;
  };

  setPredictableModeForTest();
  FakeEntropySource fakeEntropySource;
  auto sc = InvocationSpanContext::newForInvocation(kj::none, fakeEntropySource);

  // We can create an InvocationSpanContext...
  static constexpr auto kCheck = TraceId(0x2a2a2a2a2a2a2a2a, 0x2a2a2a2a2a2a2a2a);
  KJ_EXPECT(sc->getTraceId() == kCheck);
  KJ_EXPECT(sc->getInvocationId() == kCheck);
  KJ_EXPECT(sc->getSpanId() == 0);

  // And serialize that to a capnp struct...
  capnp::MallocMessageBuilder builder;
  auto root = builder.initRoot<rpc::InvocationSpanContext>();
  sc->toCapnp(root);

  // Then back again...
  auto sc2 = KJ_ASSERT_NONNULL(InvocationSpanContext::fromCapnp(root.asReader()));
  KJ_EXPECT(sc2->getTraceId() == kCheck);
  KJ_EXPECT(sc2->getInvocationId() == kCheck);
  KJ_EXPECT(sc2->getSpanId() == 0);
  KJ_EXPECT(sc2->isTrigger());

  // The one that has been deserialized from capnp cannot create children...
  try {
    sc2->newChild();
    KJ_FAIL_ASSERT("should not be able to create child span with SpanContext from capnp");
  } catch (kj::Exception& ex) {
    // KJ_EXPECT(ex.getDescription() ==
    //     "expected !isTrigger(); unable to create child spans on this context"_kj);
  }

  auto sc3 = sc->newChild();
  KJ_EXPECT(sc3->getTraceId() == kCheck);
  KJ_EXPECT(sc3->getInvocationId() == kCheck);
  KJ_EXPECT(sc3->getSpanId() == 1);

  auto sc4 = InvocationSpanContext::newForInvocation(sc2);
  KJ_EXPECT(sc4->getTraceId() == kCheck);
  KJ_EXPECT(sc4->getInvocationId() == kCheck);
  KJ_EXPECT(sc4->getSpanId() == 0);

  auto& sc5 = KJ_ASSERT_NONNULL(sc4->getParent());
  KJ_EXPECT(sc5->getTraceId() == kCheck);
  KJ_EXPECT(sc5->getInvocationId() == kCheck);
  KJ_EXPECT(sc5->getSpanId() == 0);
  KJ_EXPECT(sc5->isTrigger());
}

}  // namespace
}  // namespace workerd::tracing
