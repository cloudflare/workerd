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
  setPredictableModeForTest();
  FakeEntropySource fakeEntropySource;
  auto sc = InvocationSpanContext::newForInvocation(kj::none, fakeEntropySource);

  // In predictable mode, TraceId::fromEntropy returns deterministic but
  // call-distinct values (process-wide counter), so the traceId and invocationId
  // of independent invocations differ from each other and across tests. Capture
  // the IDs and assert the propagation chain rather than specific constants.
  auto initialTraceId = sc.getTraceId();
  auto initialInvocationId = sc.getInvocationId();
  KJ_EXPECT(sc.getSpanId() == SpanId(1));

  // And serialize that to a capnp struct...
  capnp::MallocMessageBuilder builder;
  auto root = builder.initRoot<rpc::InvocationSpanContext>();
  sc.toCapnp(root);

  // Then back again...
  auto sc2 = KJ_ASSERT_NONNULL(InvocationSpanContext::fromCapnp(root.asReader()));
  KJ_EXPECT(sc2.getTraceId() == initialTraceId);
  KJ_EXPECT(sc2.getInvocationId() == initialInvocationId);
  KJ_EXPECT(sc2.getSpanId() == SpanId(1));
  KJ_EXPECT(sc2.isTrigger());

  // The one that has been deserialized from capnp cannot create children...
  try {
    sc2.newChild();
    KJ_FAIL_ASSERT("should not be able to create child span with SpanContext from capnp");
  } catch (kj::Exception& ex) {
    KJ_EXPECT(ex.getDescription() ==
        "expected !isTrigger(); unable to create child spans on this context"_kj);
  }

  // Children inherit both the traceId and invocationId of their parent.
  auto sc3 = sc.newChild();
  KJ_EXPECT(sc3.getTraceId() == initialTraceId);
  KJ_EXPECT(sc3.getInvocationId() == initialInvocationId);
  KJ_EXPECT(sc3.getSpanId() == SpanId(2));

  // Trigger-context propagation: traceId is inherited from sc2, but the
  // invocationId is freshly generated for the new invocation.
  auto sc4 = InvocationSpanContext::newForInvocation(sc2, fakeEntropySource);
  KJ_EXPECT(sc4.getTraceId() == initialTraceId);
  KJ_EXPECT(sc4.getInvocationId() != initialInvocationId);
  KJ_EXPECT(sc4.getSpanId() == SpanId(3));

  auto& sc5 = KJ_ASSERT_NONNULL(sc4.getParent());
  KJ_EXPECT(sc5.getTraceId() == initialTraceId);
  KJ_EXPECT(sc5.getInvocationId() == initialInvocationId);
  KJ_EXPECT(sc5.getSpanId() == SpanId(1));
  KJ_EXPECT(sc5.isTrigger());
}

KJ_TEST("InvocationSpanContext propagates traceFlags from trigger") {
  setPredictableModeForTest();
  FakeEntropySource fakeEntropySource;

  // Trigger with traceFlags set — propagates to new invocation
  auto trigger = InvocationSpanContext(TraceId(1, 2), TraceId(3, 4), SpanId(5), TraceFlags(0x01));
  KJ_EXPECT(KJ_ASSERT_NONNULL(trigger.getTraceFlags()) == TraceFlags(0x01));

  auto sc = InvocationSpanContext::newForInvocation(trigger, fakeEntropySource);
  KJ_EXPECT(KJ_ASSERT_NONNULL(sc.getTraceFlags()) == TraceFlags(0x01));

  // No trigger — traceFlags is absent
  auto sc2 = InvocationSpanContext::newForInvocation(kj::none, fakeEntropySource);
  KJ_EXPECT(sc2.getTraceFlags() == kj::none);

  // newChild() propagates traceFlags
  auto child = sc.newChild();
  KJ_EXPECT(KJ_ASSERT_NONNULL(child.getTraceFlags()) == TraceFlags(0x01));

  auto child2 = sc2.newChild();
  KJ_EXPECT(child2.getTraceFlags() == kj::none);
}

KJ_TEST("InvocationSpanContext traceFlags capnp round-trip and newChild propagation") {
  setPredictableModeForTest();
  FakeEntropySource fakeEntropySource;

  // traceFlags=0x01 (sampled) survives round-trip and propagates through newChild
  auto sampled = InvocationSpanContext(TraceId(1, 2), TraceId(3, 4), SpanId(5), TraceFlags(0x01));
  capnp::MallocMessageBuilder b1;
  sampled.toCapnp(b1.initRoot<rpc::InvocationSpanContext>());
  auto rt1 =
      KJ_ASSERT_NONNULL(InvocationSpanContext::fromCapnp(b1.getRoot<rpc::InvocationSpanContext>()));
  KJ_EXPECT(KJ_ASSERT_NONNULL(rt1.getTraceFlags()) == TraceFlags(0x01));
  auto child1 = InvocationSpanContext::newForInvocation(rt1, fakeEntropySource);
  KJ_EXPECT(KJ_ASSERT_NONNULL(child1.newChild().getTraceFlags()) == TraceFlags(0x01));

  // traceFlags=0x00 (unsampled) is distinct from absent
  auto unsampled = InvocationSpanContext(TraceId(1, 2), TraceId(3, 4), SpanId(5), TraceFlags(0x00));
  capnp::MallocMessageBuilder b2;
  unsampled.toCapnp(b2.initRoot<rpc::InvocationSpanContext>());
  auto rt2 =
      KJ_ASSERT_NONNULL(InvocationSpanContext::fromCapnp(b2.getRoot<rpc::InvocationSpanContext>()));
  KJ_EXPECT(KJ_ASSERT_NONNULL(rt2.getTraceFlags()) == TraceFlags(0x00));
  auto child2 = InvocationSpanContext::newForInvocation(rt2, fakeEntropySource);
  KJ_EXPECT(KJ_ASSERT_NONNULL(child2.newChild().getTraceFlags()) == TraceFlags(0x00));

  // traceFlags absent stays absent
  auto absent = InvocationSpanContext(TraceId(1, 2), TraceId(3, 4), SpanId(5), kj::none);
  capnp::MallocMessageBuilder b3;
  absent.toCapnp(b3.initRoot<rpc::InvocationSpanContext>());
  auto rt3 =
      KJ_ASSERT_NONNULL(InvocationSpanContext::fromCapnp(b3.getRoot<rpc::InvocationSpanContext>()));
  KJ_EXPECT(rt3.getTraceFlags() == kj::none);
  auto child3 = InvocationSpanContext::newForInvocation(rt3, fakeEntropySource);
  KJ_EXPECT(child3.newChild().getTraceFlags() == kj::none);
}

KJ_TEST("SpanContext") {
  setPredictableModeForTest();
  FakeEntropySource fakeEntropySource;
  auto sc =
      SpanContext(TraceId::fromEntropy(fakeEntropySource), SpanId::fromEntropy(fakeEntropySource));

  // In predictable mode, TraceId::fromEntropy returns deterministic but
  // call-distinct values; capture the IDs and verify capnp round-trip.
  auto initialTraceId = sc.getTraceId();
  KJ_EXPECT(sc.getSpanId() == SpanId(1));
  KJ_EXPECT(sc.getTraceFlags() == kj::none);

  capnp::MallocMessageBuilder builder;
  auto root = builder.initRoot<rpc::SpanContext>();
  sc.toCapnp(root);

  auto sc2 = SpanContext::fromCapnp(root.asReader());
  KJ_EXPECT(sc2.getTraceId() == initialTraceId);
  KJ_EXPECT(sc2.getSpanId() == SpanId(1));
  KJ_EXPECT(sc2.getTraceFlags() == kj::none);
}

KJ_TEST("SpanContext traceFlags preserved through capnp when set, absent when unset") {
  auto sc = SpanContext(TraceId(1, 2), SpanId(3), TraceFlags(0x01));
  KJ_EXPECT(KJ_ASSERT_NONNULL(sc.getTraceFlags()) == TraceFlags(0x01));

  capnp::MallocMessageBuilder builder;
  auto root = builder.initRoot<rpc::SpanContext>();
  sc.toCapnp(root);

  auto sc2 = SpanContext::fromCapnp(root.asReader());
  KJ_EXPECT(sc2.getTraceId() == TraceId(1, 2));
  KJ_EXPECT(KJ_ASSERT_NONNULL(sc2.getSpanId()) == SpanId(3));
  KJ_EXPECT(KJ_ASSERT_NONNULL(sc2.getTraceFlags()) == TraceFlags(0x01));

  auto sc3 = SpanContext(TraceId(4, 5), SpanId(6));
  capnp::MallocMessageBuilder builder2;
  auto root2 = builder2.initRoot<rpc::SpanContext>();
  sc3.toCapnp(root2);

  auto sc4 = SpanContext::fromCapnp(root2.asReader());
  KJ_EXPECT(sc4.getTraceFlags() == kj::none);
}

KJ_TEST("Read/Write FetchEventInfo works") {
  capnp::MallocMessageBuilder builder;
  auto fetchInfoBuilder = builder.initRoot<rpc::Trace::FetchEventInfo>();

  kj::Vector<FetchEventInfo::Header> headers;
  headers.add(FetchEventInfo::Header(kj::str("foo"), kj::str("bar")));

  FetchEventInfo info(
      kj::HttpMethod::GET, kj::str("https://example.com"), kj::str("{}"), headers.releaseAsArray());

  info.copyTo(fetchInfoBuilder);

  auto reader = fetchInfoBuilder.asReader();

  FetchEventInfo info2(reader);
  KJ_ASSERT(info2.method == kj::HttpMethod::GET);
  KJ_ASSERT(info2.url == "https://example.com"_kj);
  KJ_ASSERT(info2.cfJson == "{}"_kj);
  KJ_ASSERT(info2.headers.size() == 1);
  KJ_ASSERT(info2.headers[0].name == "foo"_kj);
  KJ_ASSERT(info2.headers[0].value == "bar"_kj);

  FetchEventInfo info3 = info.clone();
  KJ_ASSERT(info3.method == kj::HttpMethod::GET);
  KJ_ASSERT(info3.url == "https://example.com"_kj);
  KJ_ASSERT(info3.cfJson == "{}"_kj);
  KJ_ASSERT(info3.headers.size() == 1);
  KJ_ASSERT(info3.headers[0].name == "foo"_kj);
  KJ_ASSERT(info3.headers[0].value == "bar"_kj);
}

KJ_TEST("Read/Write JsRpcEventInfo works") {
  capnp::MallocMessageBuilder builder;
  auto jsRpcInfoBuilder = builder.initRoot<rpc::Trace::JsRpcEventInfo>();

  JsRpcEventInfo info(kj::str("foo"));

  info.copyTo(jsRpcInfoBuilder);

  auto reader = jsRpcInfoBuilder.asReader();

  JsRpcEventInfo info2(reader);
  KJ_ASSERT(info2.methodName == "foo"_kj);

  JsRpcEventInfo info3 = info.clone();
  KJ_ASSERT(info3.methodName == "foo"_kj);
}

KJ_TEST("Read/Write ScheduledEventInfo workers") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::ScheduledEventInfo>();

  ScheduledEventInfo info(1.2, kj::str("foo"));

  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  ScheduledEventInfo info2(reader);
  KJ_ASSERT(info2.scheduledTime == 1.2);
  KJ_ASSERT(info2.cron == "foo"_kj);

  ScheduledEventInfo info3 = info.clone();
  KJ_ASSERT(info3.scheduledTime == 1.2);
  KJ_ASSERT(info3.cron == "foo"_kj);
}

KJ_TEST("Read/Write AlarmEventInfo works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::AlarmEventInfo>();

  AlarmEventInfo info(kj::UNIX_EPOCH);

  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  AlarmEventInfo info2(reader);
  KJ_ASSERT(info.scheduledTime == info2.scheduledTime);

  AlarmEventInfo info3 = info.clone();
  KJ_ASSERT(info.scheduledTime == info3.scheduledTime);
}

KJ_TEST("Read/Write QueueEventInfo works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::QueueEventInfo>();

  QueueEventInfo info(kj::str("foo"), 1);

  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  QueueEventInfo info2(reader);
  KJ_ASSERT(info2.queueName == "foo"_kj);
  KJ_ASSERT(info2.batchSize == 1);

  QueueEventInfo info3 = info.clone();
  KJ_ASSERT(info2.queueName == "foo"_kj);
  KJ_ASSERT(info2.batchSize == 1);
}

KJ_TEST("Read/Write EmailEventInfo works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::EmailEventInfo>();

  EmailEventInfo info(kj::str("foo"), kj::str("bar"), 1);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  EmailEventInfo info2(reader);
  KJ_ASSERT(info2.mailFrom == "foo"_kj);
  KJ_ASSERT(info2.rcptTo == "bar"_kj);
  KJ_ASSERT(info2.rawSize == 1);

  EmailEventInfo info3 = info.clone();
  KJ_ASSERT(info3.mailFrom == "foo"_kj);
  KJ_ASSERT(info3.rcptTo == "bar"_kj);
  KJ_ASSERT(info3.rawSize == 1);
}

KJ_TEST("Read/Write TraceEventInfo works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::TraceEventInfo>();

  kj::Vector<kj::Own<Trace>> items(1);
  items.add(kj::heap<Trace>(kj::none, kj::str("foo"), kj::none, kj::none, kj::none,
      kj::Array<kj::String>(), kj::none, ExecutionModel::STATELESS));

  TraceEventInfo info(items.asPtr());
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  TraceEventInfo info2(reader);
  KJ_ASSERT(info2.traces.size() == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(info2.traces[0].scriptName) == "foo"_kj);

  TraceEventInfo info3 = info.clone();
  KJ_ASSERT(info2.traces.size() == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(info2.traces[0].scriptName) == "foo"_kj);
}

KJ_TEST("Read/Write HibernatableWebSocketEventInfo works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::HibernatableWebSocketEventInfo>();

  HibernatableWebSocketEventInfo info(HibernatableWebSocketEventInfo::Message{});
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  HibernatableWebSocketEventInfo info2(reader);
  KJ_ASSERT(info2.type.is<HibernatableWebSocketEventInfo::Message>());

  HibernatableWebSocketEventInfo info3 = info.clone();
  KJ_ASSERT(info3.type.is<HibernatableWebSocketEventInfo::Message>());
}

KJ_TEST("Read/Write FetchResponseInfo works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::FetchResponseInfo>();

  FetchResponseInfo info(123);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  FetchResponseInfo info2(reader);
  KJ_ASSERT(info2.statusCode == 123);

  FetchResponseInfo info3 = info.clone();
  KJ_ASSERT(info3.statusCode == 123);
}

KJ_TEST("Read/Write DiagnosticChannelEvent works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::DiagnosticChannelEvent>();

  DiagnosticChannelEvent info(kj::UNIX_EPOCH, kj::str("foo"), kj::Array<kj::byte>());
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  DiagnosticChannelEvent info2(reader);
  KJ_ASSERT(info2.timestamp == info.timestamp);
  KJ_ASSERT(info2.channel == "foo"_kj);
  KJ_ASSERT(info2.message.size() == 0);

  DiagnosticChannelEvent info3 = info.clone();
  KJ_ASSERT(info3.timestamp == info.timestamp);
  KJ_ASSERT(info3.channel == "foo"_kj);
  KJ_ASSERT(info3.message.size() == 0);
}

KJ_TEST("Read/Write Log works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Log>();

  Log info(kj::UNIX_EPOCH, LogLevel::INFO, kj::str("foo"));
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  Log info2(reader);
  KJ_ASSERT(info.timestamp == info2.timestamp);
  KJ_ASSERT(info2.logLevel == LogLevel::INFO);
  KJ_ASSERT(info2.message == "foo"_kj);

  Log info3 = info.clone();
  KJ_ASSERT(info.timestamp == info3.timestamp);
  KJ_ASSERT(info3.logLevel == LogLevel::INFO);
  KJ_ASSERT(info3.message == "foo"_kj);
}

KJ_TEST("Read/Write Exception works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Exception>();

  Exception info(kj::UNIX_EPOCH, kj::str("foo"), kj::str("bar"), kj::none);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  Exception info2(reader);
  KJ_ASSERT(info.timestamp == info2.timestamp);
  KJ_ASSERT(info2.name == "foo"_kj);
  KJ_ASSERT(info2.message == "bar"_kj);
  KJ_ASSERT(info2.stack == kj::none);

  Exception info3 = info.clone();
  KJ_ASSERT(info.timestamp == info3.timestamp);
  KJ_ASSERT(info3.name == "foo"_kj);
  KJ_ASSERT(info3.message == "bar"_kj);
  KJ_ASSERT(info3.stack == kj::none);
}

KJ_TEST("Read/Write StreamDiagnosticsEvent works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::StreamDiagnosticsEvent>();

  StreamDiagnosticsEvent info(42);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  StreamDiagnosticsEvent info2(reader);
  KJ_ASSERT(info2.droppedEventsCount == 42);

  StreamDiagnosticsEvent info3 = info.clone();
  KJ_ASSERT(info3.droppedEventsCount == 42);
}

KJ_TEST("Read/Write Attribute works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Attribute>();

  Attribute attr("foo"_kjc, {123.0, 321.2});
  attr.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  Attribute info2(reader);
  KJ_ASSERT(info2.name == "foo"_kj);
  KJ_ASSERT(KJ_ASSERT_NONNULL(info2.value[0].tryGet<double>()) == 123.0);
  KJ_ASSERT(KJ_ASSERT_NONNULL(info2.value[1].tryGet<double>()) == 321.2);
}

KJ_TEST("Read/Write Return works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Return>();

  FetchResponseInfo fetchInfo(123);
  Return info(kj::mv(fetchInfo));
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  Return info2(reader);
  auto& fetchInfo2 = KJ_ASSERT_NONNULL(info2.info);
  KJ_ASSERT(fetchInfo2.statusCode == 123);

  Return info3 = info.clone();
  auto& fetchInfo3 = KJ_ASSERT_NONNULL(info3.info);
  KJ_ASSERT(fetchInfo3.statusCode == 123);
}

KJ_TEST("Read/Write SpanOpen works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::SpanOpen>();

  SpanOpen info(0x2a2a2a2a2a2a2a2a, "foo"_kjc, kj::none);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  SpanOpen info2(reader);
  KJ_ASSERT(info2.operationName == "foo"_kj);
  KJ_ASSERT(info2.info == kj::none);

  SpanOpen info3 = info.clone();
  KJ_ASSERT(info3.operationName == "foo"_kj);
  KJ_ASSERT(info3.info == kj::none);
}

KJ_TEST("Read/Write SpanClose works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::SpanClose>();

  SpanClose info(EventOutcome::EXCEPTION);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  SpanClose info2(reader);
  KJ_ASSERT(info2.outcome == EventOutcome::EXCEPTION);

  SpanClose info3 = info.clone();
  KJ_ASSERT(info3.outcome == EventOutcome::EXCEPTION);
}

KJ_TEST("Read/Write Onset works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Onset>();

  FetchEventInfo fetchInfo(
      kj::HttpMethod::GET, kj::str("https://example.com"), kj::str("{}"), nullptr);

  Onset info(staticSpanId, Onset::Info(kj::mv(fetchInfo)),
      {
        .scriptName = kj::str("foo"),
        .preview = TracePreview(kj::str("63bafce9179948688866bb22268eb1c6"),
            kj::str("feature-my-branch"), kj::str("feature/my-branch")),
      },
      nullptr);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  Onset info2(reader);
  FetchEventInfo& fetchInfo2 = KJ_ASSERT_NONNULL(info2.info.tryGet<FetchEventInfo>());
  KJ_ASSERT(fetchInfo2.method == kj::HttpMethod::GET);
  KJ_ASSERT(fetchInfo2.url == "https://example.com"_kj);
  KJ_ASSERT(info2.workerInfo.executionModel == ExecutionModel::STATELESS);
  auto& preview2 = KJ_ASSERT_NONNULL(info2.workerInfo.preview);
  KJ_ASSERT(preview2.id == "63bafce9179948688866bb22268eb1c6"_kj);
  KJ_ASSERT(preview2.slug == "feature-my-branch"_kj);
  KJ_ASSERT(preview2.name == "feature/my-branch"_kj);

  Onset info3 = info.clone();
  FetchEventInfo& fetchInfo3 = KJ_ASSERT_NONNULL(info3.info.tryGet<FetchEventInfo>());
  KJ_ASSERT(fetchInfo3.method == kj::HttpMethod::GET);
  KJ_ASSERT(fetchInfo3.url == "https://example.com"_kj);
  KJ_ASSERT(info3.workerInfo.executionModel == ExecutionModel::STATELESS);
  auto& preview3 = KJ_ASSERT_NONNULL(info3.workerInfo.preview);
  KJ_ASSERT(preview3.id == "63bafce9179948688866bb22268eb1c6"_kj);
  KJ_ASSERT(preview3.slug == "feature-my-branch"_kj);
  KJ_ASSERT(preview3.name == "feature/my-branch"_kj);
}

KJ_TEST("Read/Write Outcome works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Outcome>();

  Outcome info(EventOutcome::EXCEPTION, 1 * kj::MILLISECONDS, 2 * kj::MILLISECONDS);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  Outcome info2(reader);
  KJ_ASSERT(info2.outcome == EventOutcome::EXCEPTION);
  KJ_ASSERT(info2.wallTime == 2 * kj::MILLISECONDS);
  KJ_ASSERT(info2.cpuTime == 1 * kj::MILLISECONDS);

  Outcome info3 = info.clone();
  KJ_ASSERT(info3.outcome == EventOutcome::EXCEPTION);
  KJ_ASSERT(info3.wallTime == 2 * kj::MILLISECONDS);
  KJ_ASSERT(info3.cpuTime == 1 * kj::MILLISECONDS);
}

KJ_TEST("Read/Write TailEvent works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::TailEvent>();

  auto context = SpanContext(TraceId(0, 0), {staticSpanId});
  Log log(kj::UNIX_EPOCH, LogLevel::INFO, kj::str("foo"));
  auto invocationId = TraceId(0, 0);
  TailEvent info(
      context.getTraceId(), invocationId, context.getSpanId(), kj::UNIX_EPOCH, 0, kj::mv(log));
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  TailEvent info2(reader);
  KJ_ASSERT(info2.timestamp == kj::UNIX_EPOCH);
  KJ_ASSERT(info2.sequence == 0);
  KJ_ASSERT(info2.invocationId == invocationId);
  KJ_ASSERT(info2.spanContext == context);

  auto& log2 = KJ_ASSERT_NONNULL(info2.event.tryGet<Log>());
  KJ_ASSERT(log2.timestamp == kj::UNIX_EPOCH);
  KJ_ASSERT(log2.logLevel == LogLevel::INFO);
  KJ_ASSERT(log2.message == "foo"_kj);

  TailEvent info3 = info.clone();
  KJ_ASSERT(info3.timestamp == kj::UNIX_EPOCH);
  KJ_ASSERT(info3.sequence == 0);
  KJ_ASSERT(info3.invocationId == invocationId);
  KJ_ASSERT(info3.spanContext == context);

  auto& log3 = KJ_ASSERT_NONNULL(info3.event.tryGet<Log>());
  KJ_ASSERT(log3.timestamp == kj::UNIX_EPOCH);
  KJ_ASSERT(log3.logLevel == LogLevel::INFO);
  KJ_ASSERT(log3.message == "foo"_kj);
}

KJ_TEST("Read/Write TailEvent with Multiple Attributes") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::TailEvent>();

  TraceId traceId(0, 0);
  auto context = SpanContext(traceId, {staticSpanId});

  // An attribute event can have one or more Attributes specified.
  kj::Vector<Attribute> attrs(2);
  attrs.add(Attribute("foo"_kjc, true));
  attrs.add(Attribute("bar"_kjc, static_cast<int64_t>(123)));

  TailEvent info(kj::mv(context), traceId, kj::UNIX_EPOCH, 0, attrs.releaseAsArray());
  info.copyTo(infoBuilder);

  TailEvent info2(infoBuilder.asReader());
  auto& attrs2 = KJ_ASSERT_NONNULL(info2.event.tryGet<kj::Array<Attribute>>());
  KJ_ASSERT(attrs2.size() == 2);

  KJ_ASSERT(attrs2[0].name == "foo"_kj);
  KJ_ASSERT(attrs2[1].name == "bar"_kj);
}

KJ_TEST("Trace with Preview") {
  auto trace = kj::refcounted<Trace>(kj::str("test-stable-id"), kj::str("test-script"),
      kj::none,  // scriptVersion
      kj::str("test-namespace"), kj::str("test-script-id"),
      kj::Array<kj::String>(),  // scriptTags
      kj::str("test-entrypoint"), ExecutionModel::STATELESS,
      kj::none,  // durableObjectId
      TracePreview(kj::str("63bafce9179948688866bb22268eb1c6"), kj::str("feature-my-branch"),
          kj::str("feature/my-branch")));

  capnp::MallocMessageBuilder builder;
  auto traceBuilder = builder.initRoot<rpc::Trace>();
  trace->copyTo(traceBuilder);

  auto trace2 = kj::refcounted<Trace>(traceBuilder.asReader());
  auto& preview = KJ_ASSERT_NONNULL(trace2->preview);
  KJ_ASSERT(preview.id == "63bafce9179948688866bb22268eb1c6"_kj);
  KJ_ASSERT(preview.slug == "feature-my-branch"_kj);
  KJ_ASSERT(preview.name == "feature/my-branch"_kj);
}

KJ_TEST("Trace with Durable Object ID") {
  auto trace = kj::refcounted<Trace>(kj::str("test-stable-id"), kj::str("test-script"),
      kj::none,  // scriptVersion
      kj::str("test-namespace"), kj::str("test-script-id"),
      kj::Array<kj::String>(),  // scriptTags
      kj::str("test-entrypoint"), ExecutionModel::DURABLE_OBJECT,
      kj::str("abc123def456")  // durableObjectId
  );

  capnp::MallocMessageBuilder builder;
  auto traceBuilder = builder.initRoot<rpc::Trace>();
  trace->copyTo(traceBuilder);

  auto trace2 = kj::refcounted<Trace>(traceBuilder.asReader());
  KJ_ASSERT(KJ_REQUIRE_NONNULL(trace2->durableObjectId) == "abc123def456"_kj);
}
KJ_TEST("SpanContext::tryFromTraceparent valid") {
  auto result = KJ_ASSERT_NONNULL(SpanContext::tryFromTraceparent(
      "00-11223344556677889900aabbccddeeff-a1b2c3d4e5f60718-01"_kj));
  KJ_EXPECT(result.getTraceId() == TraceId(0x9900aabbccddeeff, 0x1122334455667788));
  KJ_EXPECT(KJ_ASSERT_NONNULL(result.getSpanId()) == SpanId(0xa1b2c3d4e5f60718));
  KJ_EXPECT(KJ_ASSERT_NONNULL(result.getTraceFlags()) == 0x01);
}

KJ_TEST("SpanContext::tryFromTraceparent sampled with extra flags") {
  auto result = KJ_ASSERT_NONNULL(SpanContext::tryFromTraceparent(
      "00-11223344556677889900aabbccddeeff-a1b2c3d4e5f60718-03"_kj));
  KJ_EXPECT(result.getTraceId() == TraceId(0x9900aabbccddeeff, 0x1122334455667788));
  KJ_EXPECT(KJ_ASSERT_NONNULL(result.getSpanId()) == SpanId(0xa1b2c3d4e5f60718));
  KJ_EXPECT(KJ_ASSERT_NONNULL(result.getTraceFlags()) == 0x03);
}

KJ_TEST("SpanContext::tryFromTraceparent rejects invalid inputs") {
  // Empty
  KJ_EXPECT(SpanContext::tryFromTraceparent(""_kj) == kj::none);
  // Degenerate
  KJ_EXPECT(SpanContext::tryFromTraceparent("---"_kj) == kj::none);
  KJ_EXPECT(SpanContext::tryFromTraceparent("00-1-1-00"_kj) == kj::none);

  // Wrong total length (too short, too long)
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "00-11223344556677889900aabbccddeeff-a1b2c3d4e5f60718-0"_kj) == kj::none);
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "00-11223344556677889900aabbccddeeff-a1b2c3d4e5f60718-012"_kj) == kj::none);

  // Wrong field sizes: version too short
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "0-11223344556677889900aabbccddeeff0-a1b2c3d4e5f60718-01"_kj) == kj::none);
  // Wrong field sizes: trace-id too long
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "00-11223344556677889900aabbccddeeff0-1b2c3d4e5f60718-01"_kj) == kj::none);
  // Wrong field sizes: trace-id too short
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "00-11223344556677889900aabbccddee-a1b2c3d4e5f6071800-01"_kj) == kj::none);
  // Wrong field sizes: parent-id too long
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "00-1223344556677889900aabbccddeeff-a1b2c3d4e5f607180-01"_kj) == kj::none);
  // Wrong field sizes: parent-id too short
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "00-112233445566778899900aabbccddeeff-1b2c3d4e5f6071-01"_kj) == kj::none);
  // Empty fields
  KJ_EXPECT(SpanContext::tryFromTraceparent("00--a1b2c3d4e5f60718-01"_kj) == kj::none);
  KJ_EXPECT(
      SpanContext::tryFromTraceparent("00-11223344556677889900aabbccddeeff--01"_kj) == kj::none);

  // Bad hex
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "0g-11223344556677889900aabbccddeeff-a1b2c3d4e5f60718-01"_kj) == kj::none);
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "00-x1223344556677889900aabbccddeeff-a1b2c3d4e5f60718-01"_kj) == kj::none);
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "00-11223344556677889900aabbccddeeff-a1b2c3d4e5f6071x-01"_kj) == kj::none);

  // Unsupported version
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "01-11223344556677889900aabbccddeeff-a1b2c3d4e5f60718-01"_kj) == kj::none);
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "ff-11223344556677889900aabbccddeeff-a1b2c3d4e5f60718-01"_kj) == kj::none);

  // All-zero trace-id
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "00-00000000000000000000000000000000-a1b2c3d4e5f60718-01"_kj) == kj::none);

  // All-zero parent-id
  KJ_EXPECT(SpanContext::tryFromTraceparent(
                "00-11223344556677889900aabbccddeeff-0000000000000000-01"_kj) == kj::none);

  // Unsampled
  auto unsampled = KJ_ASSERT_NONNULL(SpanContext::tryFromTraceparent(
      "00-11223344556677889900aabbccddeeff-a1b2c3d4e5f60718-00"_kj));
  KJ_EXPECT(unsampled.getTraceId() == TraceId(0x9900aabbccddeeff, 0x1122334455667788));
  KJ_EXPECT(KJ_ASSERT_NONNULL(unsampled.getSpanId()) == SpanId(0xa1b2c3d4e5f60718));
  KJ_EXPECT(KJ_ASSERT_NONNULL(unsampled.getTraceFlags()) == 0x00);
}

}  // namespace
}  // namespace workerd::tracing
