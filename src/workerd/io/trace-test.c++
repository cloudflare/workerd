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

  // We can create an InvocationSpanContext...
  static constexpr auto kCheck = TraceId(0x2a2a2a2a2a2a2a2a, 0x2a2a2a2a2a2a2a2a);
  KJ_EXPECT(sc.getTraceId() == kCheck);
  KJ_EXPECT(sc.getInvocationId() == kCheck);
  KJ_EXPECT(sc.getSpanId() == SpanId(1));

  // And serialize that to a capnp struct...
  capnp::MallocMessageBuilder builder;
  auto root = builder.initRoot<rpc::InvocationSpanContext>();
  sc.toCapnp(root);

  // Then back again...
  auto sc2 = KJ_ASSERT_NONNULL(InvocationSpanContext::fromCapnp(root.asReader()));
  KJ_EXPECT(sc2.getTraceId() == kCheck);
  KJ_EXPECT(sc2.getInvocationId() == kCheck);
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

  auto sc3 = sc.newChild();
  KJ_EXPECT(sc3.getTraceId() == kCheck);
  KJ_EXPECT(sc3.getInvocationId() == kCheck);
  KJ_EXPECT(sc3.getSpanId() == SpanId(2));

  auto sc4 = InvocationSpanContext::newForInvocation(sc2, fakeEntropySource);
  KJ_EXPECT(sc4.getTraceId() == kCheck);
  KJ_EXPECT(sc4.getInvocationId() == kCheck);
  KJ_EXPECT(sc4.getSpanId() == SpanId(3));

  auto& sc5 = KJ_ASSERT_NONNULL(sc4.getParent());
  KJ_EXPECT(sc5.getTraceId() == kCheck);
  KJ_EXPECT(sc5.getInvocationId() == kCheck);
  KJ_EXPECT(sc5.getSpanId() == SpanId(1));
  KJ_EXPECT(sc5.isTrigger());
}

KJ_TEST("Read/Write FetchEventInfo works") {
  capnp::MallocMessageBuilder builder;
  auto fetchInfoBuilder = builder.initRoot<rpc::Trace::FetchEventInfo>();

  kj::Vector<FetchEventInfo::Header> headers;
  headers.add(FetchEventInfo::Header(kj::str("foo"), kj::str("bar")));

  tracing::FetchEventInfo info(
      kj::HttpMethod::GET, kj::str("https://example.com"), kj::str("{}"), headers.releaseAsArray());

  info.copyTo(fetchInfoBuilder);

  auto reader = fetchInfoBuilder.asReader();

  tracing::FetchEventInfo info2(reader);
  KJ_ASSERT(info2.method == kj::HttpMethod::GET);
  KJ_ASSERT(info2.url == "https://example.com"_kj);
  KJ_ASSERT(info2.cfJson == "{}"_kj);
  KJ_ASSERT(info2.headers.size() == 1);
  KJ_ASSERT(info2.headers[0].name == "foo"_kj);
  KJ_ASSERT(info2.headers[0].value == "bar"_kj);

  tracing::FetchEventInfo info3 = info.clone();
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

  tracing::JsRpcEventInfo info(kj::str("foo"));

  info.copyTo(jsRpcInfoBuilder);

  auto reader = jsRpcInfoBuilder.asReader();

  tracing::JsRpcEventInfo info2(reader);
  KJ_ASSERT(info2.methodName == "foo"_kj);

  tracing::JsRpcEventInfo info3 = info.clone();
  KJ_ASSERT(info3.methodName == "foo"_kj);
}

KJ_TEST("Read/Write ScheduledEventInfo workers") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::ScheduledEventInfo>();

  tracing::ScheduledEventInfo info(1.2, kj::str("foo"));

  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  tracing::ScheduledEventInfo info2(reader);
  KJ_ASSERT(info2.scheduledTime == 1.2);
  KJ_ASSERT(info2.cron == "foo"_kj);

  tracing::ScheduledEventInfo info3 = info.clone();
  KJ_ASSERT(info3.scheduledTime == 1.2);
  KJ_ASSERT(info3.cron == "foo"_kj);
}

KJ_TEST("Read/Write AlarmEventInfo works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::AlarmEventInfo>();

  tracing::AlarmEventInfo info(kj::UNIX_EPOCH);

  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  tracing::AlarmEventInfo info2(reader);
  KJ_ASSERT(info.scheduledTime == info2.scheduledTime);

  tracing::AlarmEventInfo info3 = info.clone();
  KJ_ASSERT(info.scheduledTime == info3.scheduledTime);
}

KJ_TEST("Read/Write QueueEventInfo works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::QueueEventInfo>();

  tracing::QueueEventInfo info(kj::str("foo"), 1);

  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  tracing::QueueEventInfo info2(reader);
  KJ_ASSERT(info2.queueName == "foo"_kj);
  KJ_ASSERT(info2.batchSize == 1);

  tracing::QueueEventInfo info3 = info.clone();
  KJ_ASSERT(info2.queueName == "foo"_kj);
  KJ_ASSERT(info2.batchSize == 1);
}

KJ_TEST("Read/Write EmailEventInfo works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::EmailEventInfo>();

  tracing::EmailEventInfo info(kj::str("foo"), kj::str("bar"), 1);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  tracing::EmailEventInfo info2(reader);
  KJ_ASSERT(info2.mailFrom == "foo"_kj);
  KJ_ASSERT(info2.rcptTo == "bar"_kj);
  KJ_ASSERT(info2.rawSize == 1);

  tracing::EmailEventInfo info3 = info.clone();
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

  tracing::TraceEventInfo info(items.asPtr());
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  tracing::TraceEventInfo info2(reader);
  KJ_ASSERT(info2.traces.size() == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(info2.traces[0].scriptName) == "foo"_kj);

  tracing::TraceEventInfo info3 = info.clone();
  KJ_ASSERT(info2.traces.size() == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(info2.traces[0].scriptName) == "foo"_kj);
}

KJ_TEST("Read/Write HibernatableWebSocketEventInfo works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::HibernatableWebSocketEventInfo>();

  tracing::HibernatableWebSocketEventInfo info(tracing::HibernatableWebSocketEventInfo::Message{});
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  tracing::HibernatableWebSocketEventInfo info2(reader);
  KJ_ASSERT(info2.type.is<tracing::HibernatableWebSocketEventInfo::Message>());

  tracing::HibernatableWebSocketEventInfo info3 = info.clone();
  KJ_ASSERT(info3.type.is<tracing::HibernatableWebSocketEventInfo::Message>());
}

KJ_TEST("Read/Write FetchResponseInfo works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::FetchResponseInfo>();

  tracing::FetchResponseInfo info(123);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  tracing::FetchResponseInfo info2(reader);
  KJ_ASSERT(info2.statusCode == 123);

  tracing::FetchResponseInfo info3 = info.clone();
  KJ_ASSERT(info3.statusCode == 123);
}

KJ_TEST("Read/Write DiagnosticChannelEvent works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::DiagnosticChannelEvent>();

  tracing::DiagnosticChannelEvent info(kj::UNIX_EPOCH, kj::str("foo"), kj::Array<kj::byte>());
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  tracing::DiagnosticChannelEvent info2(reader);
  KJ_ASSERT(info2.timestamp == info.timestamp);
  KJ_ASSERT(info2.channel == "foo"_kj);
  KJ_ASSERT(info2.message.size() == 0);

  tracing::DiagnosticChannelEvent info3 = info.clone();
  KJ_ASSERT(info3.timestamp == info.timestamp);
  KJ_ASSERT(info3.channel == "foo"_kj);
  KJ_ASSERT(info3.message.size() == 0);
}

KJ_TEST("Read/Write Log works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Log>();

  tracing::Log info(kj::UNIX_EPOCH, LogLevel::INFO, kj::str("foo"));
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  tracing::Log info2(reader);
  KJ_ASSERT(info.timestamp == info2.timestamp);
  KJ_ASSERT(info2.logLevel == LogLevel::INFO);
  KJ_ASSERT(info2.message == "foo"_kj);

  tracing::Log info3 = info.clone();
  KJ_ASSERT(info.timestamp == info3.timestamp);
  KJ_ASSERT(info3.logLevel == LogLevel::INFO);
  KJ_ASSERT(info3.message == "foo"_kj);
}

KJ_TEST("Read/Write Exception works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Exception>();

  tracing::Exception info(kj::UNIX_EPOCH, kj::str("foo"), kj::str("bar"), kj::none);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  tracing::Exception info2(reader);
  KJ_ASSERT(info.timestamp == info2.timestamp);
  KJ_ASSERT(info2.name == "foo"_kj);
  KJ_ASSERT(info2.message == "bar"_kj);
  KJ_ASSERT(info2.stack == kj::none);

  tracing::Exception info3 = info.clone();
  KJ_ASSERT(info.timestamp == info3.timestamp);
  KJ_ASSERT(info3.name == "foo"_kj);
  KJ_ASSERT(info3.message == "bar"_kj);
  KJ_ASSERT(info3.stack == kj::none);
}

KJ_TEST("Read/Write Resume works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Resume>();

  tracing::Resume info(kj::arr<kj::byte>(1, 2, 3));
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  tracing::Resume info2(reader);
  auto& attachment = KJ_ASSERT_NONNULL(info2.attachment);
  KJ_ASSERT(attachment.size() == 3);
  KJ_ASSERT(attachment[0] == 1);
  KJ_ASSERT(attachment[1] == 2);
  KJ_ASSERT(attachment[2] == 3);

  tracing::Resume info3 = info.clone();
  auto& attachment2 = KJ_ASSERT_NONNULL(info3.attachment);
  KJ_ASSERT(attachment2.size() == 3);
  KJ_ASSERT(attachment2[0] == 1);
  KJ_ASSERT(attachment2[1] == 2);
  KJ_ASSERT(attachment2[2] == 3);
}

KJ_TEST("Read/Write Hibernate works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Hibernate>();

  tracing::Hibernate info;
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  tracing::Hibernate info2(reader);

  info.clone();
}

KJ_TEST("Read/Write Attribute works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Attribute>();

  tracing::Attribute attr(kj::str("foo"), {123.0, 321.2});
  attr.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  tracing::Attribute info2(reader);
  KJ_ASSERT(info2.name == "foo"_kj);
  KJ_ASSERT(KJ_ASSERT_NONNULL(info2.value[0].tryGet<double>()) == 123.0);
  KJ_ASSERT(KJ_ASSERT_NONNULL(info2.value[1].tryGet<double>()) == 321.2);
}

KJ_TEST("Read/Write Return works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Return>();

  tracing::FetchResponseInfo fetchInfo(123);
  tracing::Return info(tracing::Return::Info(kj::mv(fetchInfo)));
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  tracing::Return info2(reader);
  auto& fetchInfo2 =
      KJ_ASSERT_NONNULL(KJ_ASSERT_NONNULL(info2.info).tryGet<tracing::FetchResponseInfo>());
  KJ_ASSERT(fetchInfo2.statusCode == 123);

  tracing::Return info3 = info.clone();
  auto& fetchInfo3 =
      KJ_ASSERT_NONNULL(KJ_ASSERT_NONNULL(info3.info).tryGet<tracing::FetchResponseInfo>());
  KJ_ASSERT(fetchInfo3.statusCode == 123);
}

KJ_TEST("Read/Write SpanOpen works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::SpanOpen>();

  tracing::SpanOpen info(kj::str("foo"), kj::none);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  tracing::SpanOpen info2(reader);
  KJ_ASSERT(KJ_ASSERT_NONNULL(info2.operationName) == "foo"_kj);
  KJ_ASSERT(info2.info == kj::none);

  tracing::SpanOpen info3 = info.clone();
  KJ_ASSERT(KJ_ASSERT_NONNULL(info3.operationName) == "foo"_kj);
  KJ_ASSERT(info3.info == kj::none);
}

KJ_TEST("Read/Write SpanClose works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::SpanClose>();

  tracing::SpanClose info(EventOutcome::EXCEPTION);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  tracing::SpanClose info2(reader);
  KJ_ASSERT(info2.outcome == EventOutcome::EXCEPTION);

  tracing::SpanClose info3 = info.clone();
  KJ_ASSERT(info3.outcome == EventOutcome::EXCEPTION);
}

KJ_TEST("Read/Write Onset works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Onset>();

  tracing::FetchEventInfo fetchInfo(
      kj::HttpMethod::GET, kj::str("https://example.com"), kj::str("{}"), nullptr);
  tracing::Onset info(tracing::Onset::Info(kj::mv(fetchInfo)), ExecutionModel::STATELESS,
      kj::str("foo"), kj::none, kj::none, kj::none, kj::none);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  tracing::Onset info2(reader);
  tracing::FetchEventInfo& fetchInfo2 =
      KJ_ASSERT_NONNULL(info2.info.tryGet<tracing::FetchEventInfo>());
  KJ_ASSERT(fetchInfo2.method == kj::HttpMethod::GET);
  KJ_ASSERT(fetchInfo2.url == "https://example.com"_kj);
  KJ_ASSERT(info2.executionModel == ExecutionModel::STATELESS);

  tracing::Onset info3 = info.clone();
  tracing::FetchEventInfo& fetchInfo3 =
      KJ_ASSERT_NONNULL(info3.info.tryGet<tracing::FetchEventInfo>());
  KJ_ASSERT(fetchInfo3.method == kj::HttpMethod::GET);
  KJ_ASSERT(fetchInfo3.url == "https://example.com"_kj);
  KJ_ASSERT(info3.executionModel == ExecutionModel::STATELESS);
}

KJ_TEST("Read/Write Outcome works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::Outcome>();

  tracing::Outcome info(EventOutcome::EXCEPTION, 1 * kj::MILLISECONDS, 2 * kj::MILLISECONDS);
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();
  tracing::Outcome info2(reader);
  KJ_ASSERT(info2.outcome == EventOutcome::EXCEPTION);
  KJ_ASSERT(info2.wallTime == 2 * kj::MILLISECONDS);
  KJ_ASSERT(info2.cpuTime == 1 * kj::MILLISECONDS);

  tracing::Outcome info3 = info.clone();
  KJ_ASSERT(info3.outcome == EventOutcome::EXCEPTION);
  KJ_ASSERT(info3.wallTime == 2 * kj::MILLISECONDS);
  KJ_ASSERT(info3.cpuTime == 1 * kj::MILLISECONDS);
}

KJ_TEST("Read/Write TailEvent works") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::TailEvent>();

  FakeEntropySource entropy;
  auto context = tracing::InvocationSpanContext::newForInvocation(kj::none, entropy);
  tracing::Log log(kj::UNIX_EPOCH, LogLevel::INFO, kj::str("foo"));
  tracing::TailEvent info(context, kj::UNIX_EPOCH, 0, tracing::Mark(kj::mv(log)));
  info.copyTo(infoBuilder);

  auto reader = infoBuilder.asReader();

  tracing::TailEvent info2(reader);
  KJ_ASSERT(info2.timestamp == kj::UNIX_EPOCH);
  KJ_ASSERT(info2.sequence == 0);
  KJ_ASSERT(info2.invocationId == context.getInvocationId());
  KJ_ASSERT(info2.traceId == context.getTraceId());
  KJ_ASSERT(info2.spanId == context.getSpanId());

  auto& event = KJ_ASSERT_NONNULL(info2.event.tryGet<tracing::Mark>());
  auto& log2 = KJ_ASSERT_NONNULL(event.tryGet<tracing::Log>());
  KJ_ASSERT(log2.timestamp == kj::UNIX_EPOCH);
  KJ_ASSERT(log2.logLevel == LogLevel::INFO);
  KJ_ASSERT(log2.message == "foo"_kj);

  tracing::TailEvent info3 = info.clone();
  KJ_ASSERT(info3.timestamp == kj::UNIX_EPOCH);
  KJ_ASSERT(info3.sequence == 0);
  KJ_ASSERT(info3.invocationId == context.getInvocationId());
  KJ_ASSERT(info3.traceId == context.getTraceId());
  KJ_ASSERT(info3.spanId == context.getSpanId());

  auto& event2 = KJ_ASSERT_NONNULL(info3.event.tryGet<tracing::Mark>());
  auto& log3 = KJ_ASSERT_NONNULL(event2.tryGet<tracing::Log>());
  KJ_ASSERT(log3.timestamp == kj::UNIX_EPOCH);
  KJ_ASSERT(log3.logLevel == LogLevel::INFO);
  KJ_ASSERT(log3.message == "foo"_kj);
}

KJ_TEST("Read/Write TailEvent with Multiple Attributes") {
  capnp::MallocMessageBuilder builder;
  auto infoBuilder = builder.initRoot<rpc::Trace::TailEvent>();

  FakeEntropySource entropy;
  auto context = tracing::InvocationSpanContext::newForInvocation(kj::none, entropy);

  // An attribute event can have one or more Attributes specified.
  kj::Vector<tracing::Attribute> attrs(2);
  attrs.add(tracing::Attribute(kj::str("foo"), true));
  attrs.add(tracing::Attribute(kj::str("bar"), 123));

  tracing::TailEvent info(context, kj::UNIX_EPOCH, 0, tracing::Mark(attrs.releaseAsArray()));
  info.copyTo(infoBuilder);

  tracing::TailEvent info2(infoBuilder.asReader());
  auto& mark = KJ_ASSERT_NONNULL(info2.event.tryGet<tracing::Mark>());
  auto& attrs2 = KJ_ASSERT_NONNULL(mark.tryGet<kj::Array<tracing::Attribute>>());
  KJ_ASSERT(attrs2.size() == 2);

  KJ_ASSERT(attrs2[0].name == "foo"_kj);
  KJ_ASSERT(attrs2[1].name == "bar"_kj);
}

}  // namespace
}  // namespace workerd::tracing
