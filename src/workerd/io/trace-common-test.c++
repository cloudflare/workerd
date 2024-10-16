#include "trace-common.h"

#include <workerd/jsg/jsg-test.h>

#include <capnp/message.h>
#include <kj/compat/http.h>
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("Tags work") {
  {
    trace::Tag tag(kj::str("a"), static_cast<uint64_t>(1));
    auto& key = KJ_ASSERT_NONNULL(tag.key.tryGet<kj::String>());
    auto& value = KJ_ASSERT_NONNULL(tag.value.tryGet<uint64_t>());
    KJ_EXPECT(key == "a");
    KJ_EXPECT(value == static_cast<uint64_t>(1));
    KJ_EXPECT(tag.keyMatches("a"_kj));

    capnp::MallocMessageBuilder message;
    auto builder = message.initRoot<rpc::Trace::Tag>();
    tag.copyTo(builder);

    // Round trip serialization works
    auto reader = builder.asReader();
    trace::Tag tag2(reader);
    auto& key2 = KJ_ASSERT_NONNULL(tag.key.tryGet<kj::String>());
    auto& value2 = KJ_ASSERT_NONNULL(tag.value.tryGet<uint64_t>());
    KJ_EXPECT(key == key2);
    KJ_EXPECT(value == value2);

    auto tag3 = tag.clone();
    KJ_EXPECT(tag3.keyMatches("a"_kj));
  }

  {
    // The key can be a uint32_t
    uint32_t a = 1;
    trace::Tag tag(a, 2.0);
    auto key = KJ_ASSERT_NONNULL(tag.key.tryGet<uint32_t>());
    KJ_EXPECT(key == a);
    KJ_EXPECT(tag.keyMatches(a));
  }
}

KJ_TEST("Onset works") {
  auto tags = kj::arr(trace::Tag(kj::str("a"), static_cast<uint64_t>(1)));
  trace::Onset onset(kj::str("bar"), kj::none, kj::str("baz"), kj::str("qux"),
      kj::arr(kj::str("quux")), kj::str("corge"), ExecutionModel::STATELESS, kj::mv(tags));

  trace::FetchEventInfo info(kj::HttpMethod::GET, kj::str("http://example.org"), kj::String(),
      kj::arr(trace::FetchEventInfo::Header(kj::str("a"), kj::str("b"))));
  onset.info = kj::mv(info);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Onset>();
  onset.copyTo(builder);

  auto reader = builder.asReader();
  trace::Onset onset2(reader);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset2.scriptName) == "bar"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset2.dispatchNamespace) == "baz"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset2.scriptId) == "qux"_kj);
  KJ_EXPECT(onset2.scriptTags.size() == 1);
  KJ_EXPECT(onset2.scriptTags[0] == "quux"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset2.entrypoint) == "corge"_kj);
  KJ_EXPECT(onset2.tags.size() == 1);
  KJ_EXPECT(onset2.tags[0].keyMatches("a"_kj));

  auto& onset2Info = KJ_ASSERT_NONNULL(onset2.info);
  auto& onset2Fetch = KJ_ASSERT_NONNULL(onset2Info.tryGet<trace::FetchEventInfo>());
  KJ_EXPECT(onset2Fetch.method == kj::HttpMethod::GET);
  KJ_EXPECT(onset2Fetch.url == "http://example.org"_kj);
  KJ_EXPECT(onset2Fetch.cfJson == "");
  KJ_EXPECT(onset2Fetch.headers.size() == 1);
  KJ_EXPECT(onset2Fetch.headers[0].name == "a"_kj);
  KJ_EXPECT(onset2Fetch.headers[0].value == "b"_kj);

  auto onset3 = onset.clone();
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset3.scriptName) == "bar"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset3.dispatchNamespace) == "baz"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset3.scriptId) == "qux"_kj);
  KJ_EXPECT(onset3.scriptTags.size() == 1);
  KJ_EXPECT(onset3.scriptTags[0] == "quux"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset3.entrypoint) == "corge"_kj);
  KJ_EXPECT(onset3.tags.size() == 1);
  KJ_EXPECT(onset3.tags[0].keyMatches("a"_kj));

  auto& onset3Info = KJ_ASSERT_NONNULL(onset3.info);
  auto& onset3Fetch = KJ_ASSERT_NONNULL(onset3Info.tryGet<trace::FetchEventInfo>());
  KJ_EXPECT(onset3Fetch.method == kj::HttpMethod::GET);
  KJ_EXPECT(onset3Fetch.url == "http://example.org"_kj);
  KJ_EXPECT(onset3Fetch.cfJson == "");
  KJ_EXPECT(onset3Fetch.headers.size() == 1);
  KJ_EXPECT(onset3Fetch.headers[0].name == "a"_kj);
  KJ_EXPECT(onset3Fetch.headers[0].value == "b"_kj);
}

KJ_TEST("FetchEventInfo works") {
  trace::FetchEventInfo info(kj::HttpMethod::GET, kj::str("http://example.org"), kj::String(),
      kj::arr(trace::FetchEventInfo::Header(kj::str("a"), kj::str("b"))));
  KJ_EXPECT(info.method == kj::HttpMethod::GET);
  KJ_EXPECT(info.url == "http://example.org"_kj);
  KJ_EXPECT(info.cfJson == "");
  KJ_EXPECT(info.headers.size() == 1);
  KJ_EXPECT(info.headers[0].name == "a"_kj);
  KJ_EXPECT(info.headers[0].value == "b"_kj);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::FetchEventInfo>();
  info.copyTo(builder);

  auto reader = builder.asReader();
  trace::FetchEventInfo info2(reader);
  KJ_EXPECT(info2.method == kj::HttpMethod::GET);
  KJ_EXPECT(info2.url == "http://example.org"_kj);
  KJ_EXPECT(info2.cfJson == "");
  KJ_EXPECT(info2.headers.size() == 1);
  KJ_EXPECT(info2.headers[0].name == "a"_kj);
  KJ_EXPECT(info2.headers[0].value == "b"_kj);

  auto info3 = info.clone();
  KJ_EXPECT(info3.method == kj::HttpMethod::GET);
  KJ_EXPECT(info3.url == "http://example.org"_kj);
  KJ_EXPECT(info3.cfJson == "");
  KJ_EXPECT(info3.headers.size() == 1);
}

KJ_TEST("FetchResponseInfo works") {
  trace::FetchResponseInfo info(200);
  KJ_EXPECT(info.statusCode == 200);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::FetchResponseInfo>();
  info.copyTo(builder);

  auto reader = builder.asReader();
  trace::FetchResponseInfo info2(reader);
  KJ_EXPECT(info2.statusCode == 200);

  auto info3 = info.clone();
  KJ_EXPECT(info3.statusCode == 200);
}

KJ_TEST("JsRpcEventInfo works") {
  trace::JsRpcEventInfo info(kj::str("foo"));
  KJ_EXPECT(info.methodName == "foo"_kj);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::JsRpcEventInfo>();
  info.copyTo(builder);

  auto reader = builder.asReader();
  trace::JsRpcEventInfo info2(reader);
  KJ_EXPECT(info2.methodName == "foo"_kj);

  auto info3 = info.clone();
  KJ_EXPECT(info3.methodName == "foo"_kj);
}

KJ_TEST("ScheduledEventInfo works") {
  trace::ScheduledEventInfo info(1.0, kj::String());
  KJ_EXPECT(info.scheduledTime == 1.0);
  KJ_EXPECT(info.cron == ""_kj);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::ScheduledEventInfo>();
  info.copyTo(builder);

  auto reader = builder.asReader();
  trace::ScheduledEventInfo info2(reader);
  KJ_EXPECT(info2.scheduledTime == 1.0);
  KJ_EXPECT(info2.cron == ""_kj);

  auto info3 = info.clone();
  KJ_EXPECT(info3.scheduledTime == 1.0);
  KJ_EXPECT(info3.cron == ""_kj);
}

KJ_TEST("AlarmEventInfo works") {
  trace::AlarmEventInfo info(1 * kj::MILLISECONDS + kj::UNIX_EPOCH);
  auto date = info.scheduledTime;

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::AlarmEventInfo>();
  info.copyTo(builder);

  auto reader = builder.asReader();
  trace::AlarmEventInfo info2(reader);
  KJ_EXPECT(info2.scheduledTime == date);

  auto info3 = info.clone();
  KJ_EXPECT(info3.scheduledTime == date);
}

KJ_TEST("QueueEventInfo works") {
  uint32_t a = 1;
  trace::QueueEventInfo info(kj::str("foo"), a);
  KJ_EXPECT(info.queueName == "foo"_kj);
  KJ_EXPECT(info.batchSize == a);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::QueueEventInfo>();
  info.copyTo(builder);

  auto reader = builder.asReader();
  trace::QueueEventInfo info2(reader);
  KJ_EXPECT(info2.queueName == "foo"_kj);
  KJ_EXPECT(info2.batchSize == a);

  auto info3 = info.clone();
  KJ_EXPECT(info3.queueName == "foo"_kj);
  KJ_EXPECT(info3.batchSize == a);
}

KJ_TEST("EmailEventInfo works") {
  uint32_t a = 1;
  trace::EmailEventInfo info(kj::str("foo"), kj::str("bar"), a);
  KJ_EXPECT(info.mailFrom == "foo"_kj);
  KJ_EXPECT(info.rcptTo == "bar"_kj);
  KJ_EXPECT(info.rawSize == a);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::EmailEventInfo>();
  info.copyTo(builder);

  auto reader = builder.asReader();
  trace::EmailEventInfo info2(reader);
  KJ_EXPECT(info2.mailFrom == "foo"_kj);
  KJ_EXPECT(info2.rcptTo == "bar"_kj);
  KJ_EXPECT(info2.rawSize == a);

  auto info3 = info.clone();
  KJ_EXPECT(info3.mailFrom == "foo"_kj);
  KJ_EXPECT(info3.rcptTo == "bar"_kj);
  KJ_EXPECT(info3.rawSize == a);
}

KJ_TEST("HibernatableWebSocketEventInfo works") {
  trace::HibernatableWebSocketEventInfo info(trace::HibernatableWebSocketEventInfo::Message{});
  KJ_EXPECT(info.type.is<trace::HibernatableWebSocketEventInfo::Message>());

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::HibernatableWebSocketEventInfo>();
  info.copyTo(builder);

  auto reader = builder.asReader();
  trace::HibernatableWebSocketEventInfo info2(reader);
  KJ_EXPECT(info2.type.is<trace::HibernatableWebSocketEventInfo::Message>());

  auto info3 = info.clone();
  KJ_EXPECT(info3.type.is<trace::HibernatableWebSocketEventInfo::Message>());
}

KJ_TEST("TraceEventInfo works") {
  trace::TraceEventInfo info(kj::arr(trace::TraceEventInfo::TraceItem(kj::str("foo"))));
  KJ_EXPECT(info.traces.size() == 1);
  KJ_EXPECT(KJ_ASSERT_NONNULL(info.traces[0].scriptName) == "foo"_kj);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::TraceEventInfo>();
  info.copyTo(builder);

  auto reader = builder.asReader();
  trace::TraceEventInfo info2(reader);
  KJ_EXPECT(info2.traces.size() == 1);
  KJ_EXPECT(KJ_ASSERT_NONNULL(info2.traces[0].scriptName) == "foo"_kj);

  auto info3 = info.clone();
  KJ_EXPECT(info3.traces.size() == 1);
  KJ_EXPECT(KJ_ASSERT_NONNULL(info3.traces[0].scriptName) == "foo"_kj);
}

KJ_TEST("Outcome works") {
  trace::FetchResponseInfo info(200);

  trace::Outcome outcome(EventOutcome::OK, kj::Maybe(kj::mv(info)));
  KJ_EXPECT(outcome.outcome == EventOutcome::OK);
  KJ_EXPECT(KJ_ASSERT_NONNULL(outcome.info).is<trace::FetchResponseInfo>());

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Outcome>();
  outcome.copyTo(builder);

  auto reader = builder.asReader();
  trace::Outcome outcome2(reader);
  KJ_EXPECT(outcome2.outcome == EventOutcome::OK);
  KJ_EXPECT(KJ_ASSERT_NONNULL(outcome2.info).is<trace::FetchResponseInfo>());

  auto outcome3 = outcome.clone();
  KJ_EXPECT(outcome3.outcome == EventOutcome::OK);
  KJ_EXPECT(KJ_ASSERT_NONNULL(outcome3.info).is<trace::FetchResponseInfo>());
}

KJ_TEST("DiagnosticChannelEvent works") {
  kj::Date date = 0 * kj::MILLISECONDS + kj::UNIX_EPOCH;
  trace::DiagnosticChannelEvent event(date, kj::str("foo"), kj::arr(kj::byte(1)));
  KJ_EXPECT(event.timestamp == date);
  KJ_EXPECT(event.channel == "foo"_kj);
  KJ_EXPECT(event.message.size() == 1);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::DiagnosticChannelEvent>();
  event.copyTo(builder);

  auto reader = builder.asReader();
  trace::DiagnosticChannelEvent event2(reader);
  KJ_EXPECT(event2.timestamp == date);
  KJ_EXPECT(event2.channel == "foo"_kj);
  KJ_EXPECT(event2.message.size() == 1);

  auto event3 = event.clone();
  KJ_EXPECT(event3.timestamp == date);
  KJ_EXPECT(event3.channel == "foo"_kj);
  KJ_EXPECT(event3.message.size() == 1);
}

KJ_TEST("Log works") {
  kj::Date date = 0 * kj::MILLISECONDS + kj::UNIX_EPOCH;
  trace::Log log(date, LogLevel::INFO, kj::str("foo"));
  KJ_EXPECT(log.timestamp == date);
  KJ_EXPECT(log.logLevel == LogLevel::INFO);
  KJ_EXPECT(log.message == "foo"_kj);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Log>();
  log.copyTo(builder);

  auto reader = builder.asReader();
  trace::Log log2(reader);
  KJ_EXPECT(log2.timestamp == date);
  KJ_EXPECT(log2.logLevel == LogLevel::INFO);
  KJ_EXPECT(log2.message == "foo"_kj);

  auto log3 = log.clone();
  KJ_EXPECT(log3.timestamp == date);
  KJ_EXPECT(log3.logLevel == LogLevel::INFO);
  KJ_EXPECT(log3.message == "foo"_kj);
}

KJ_TEST("LogV2 works") {
  kj::Date date = 0 * kj::MILLISECONDS + kj::UNIX_EPOCH;
  trace::LogV2 log(date, LogLevel::INFO, kj::heapArray<kj::byte>(1));
  KJ_EXPECT(log.timestamp == date);
  KJ_EXPECT(log.logLevel == LogLevel::INFO);
  KJ_EXPECT(KJ_ASSERT_NONNULL(log.message.tryGet<kj::Array<kj::byte>>()).size() == 1);
  KJ_EXPECT(!log.truncated);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::LogV2>();
  log.copyTo(builder);

  auto reader = builder.asReader();
  trace::LogV2 log2(reader);
  KJ_EXPECT(log2.timestamp == date);
  KJ_EXPECT(log2.logLevel == LogLevel::INFO);
  KJ_EXPECT(KJ_ASSERT_NONNULL(log2.message.tryGet<kj::Array<kj::byte>>()).size() == 1);
  KJ_EXPECT(!log2.truncated);

  auto log3 = log.clone();
  KJ_EXPECT(log3.timestamp == date);
  KJ_EXPECT(log3.logLevel == LogLevel::INFO);
  KJ_EXPECT(KJ_ASSERT_NONNULL(log3.message.tryGet<kj::Array<kj::byte>>()).size() == 1);
  KJ_EXPECT(!log3.truncated);
}

KJ_TEST("Exception works") {
  kj::Date date = 0 * kj::MILLISECONDS + kj::UNIX_EPOCH;
  trace::Exception exception(date, kj::str("foo"), kj::str("bar"), kj::none);
  KJ_EXPECT(exception.timestamp == date);
  KJ_EXPECT(exception.name == "foo"_kj);
  KJ_EXPECT(exception.message == "bar"_kj);
  KJ_EXPECT(exception.stack == nullptr);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Exception>();
  exception.copyTo(builder);

  auto reader = builder.asReader();
  trace::Exception exception2(reader);
  KJ_EXPECT(exception2.timestamp == date);
  KJ_EXPECT(exception2.name == "foo"_kj);
  KJ_EXPECT(exception2.message == "bar"_kj);
  KJ_EXPECT(exception2.stack == nullptr);

  auto exception3 = exception.clone();
  KJ_EXPECT(exception3.timestamp == date);
  KJ_EXPECT(exception3.name == "foo"_kj);
  KJ_EXPECT(exception3.message == "bar"_kj);
  KJ_EXPECT(exception3.stack == nullptr);
}

KJ_TEST("Subrequest works") {
  uint32_t a = 1;
  trace::Subrequest subrequest(a,
      trace::Subrequest::Info(trace::FetchEventInfo(
          kj::HttpMethod::GET, kj::str("http://example.org"), kj::String(), nullptr)));
  KJ_EXPECT(subrequest.id == a);
  KJ_EXPECT(KJ_ASSERT_NONNULL(subrequest.info).is<trace::FetchEventInfo>());

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Subrequest>();
  subrequest.copyTo(builder);

  auto reader = builder.asReader();
  trace::Subrequest subrequest2(reader);
  KJ_EXPECT(subrequest2.id == a);
  KJ_EXPECT(KJ_ASSERT_NONNULL(subrequest.info).is<trace::FetchEventInfo>());

  auto subrequest3 = subrequest.clone();
  KJ_EXPECT(subrequest3.id == a);
  KJ_EXPECT(KJ_ASSERT_NONNULL(subrequest.info).is<trace::FetchEventInfo>());
}

KJ_TEST("SubrequestOutcome works") {
  uint32_t a = 1;
  trace::SubrequestOutcome outcome(a, kj::none, trace::SpanClose::Outcome::OK);
  KJ_EXPECT(outcome.id == a);
  KJ_EXPECT(outcome.outcome == trace::SpanClose::Outcome::OK);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::SubrequestOutcome>();
  outcome.copyTo(builder);

  auto reader = builder.asReader();
  trace::SubrequestOutcome outcome2(reader);
  KJ_EXPECT(outcome2.id == a);
  KJ_EXPECT(outcome2.outcome == trace::SpanClose::Outcome::OK);

  auto outcome3 = outcome.clone();
  KJ_EXPECT(outcome3.id == a);
  KJ_EXPECT(outcome3.outcome == trace::SpanClose::Outcome::OK);
}

KJ_TEST("SpanClose works") {
  trace::SpanClose event(trace::SpanClose::Outcome::OK, nullptr);
  KJ_EXPECT(event.outcome == trace::SpanClose::Outcome::OK);
  KJ_EXPECT(event.tags.size() == 0);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::SpanClose>();
  event.copyTo(builder);

  auto reader = builder.asReader();
  trace::SpanClose event2(reader);
  KJ_EXPECT(event2.outcome == trace::SpanClose::Outcome::OK);
  KJ_EXPECT(event2.tags.size() == 0);

  auto event3 = event.clone();
  KJ_EXPECT(event3.outcome == trace::SpanClose::Outcome::OK);
  KJ_EXPECT(event3.tags.size() == 0);
}

KJ_TEST("Mark works") {
  trace::Mark mark(kj::str("foo"));
  KJ_EXPECT(mark.name == "foo"_kj);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Mark>();
  mark.copyTo(builder);

  auto reader = builder.asReader();
  trace::Mark mark2(reader);
  KJ_EXPECT(mark2.name == "foo"_kj);

  auto mark3 = mark.clone();
  KJ_EXPECT(mark3.name == "foo"_kj);
}

KJ_TEST("Metric works") {
  trace::Metric metric(trace::Metric::Type::COUNTER, kj::str("foo"), 1.0);
  KJ_EXPECT(metric.type == trace::Metric::Type::COUNTER);
  KJ_EXPECT(KJ_ASSERT_NONNULL(metric.key.tryGet<kj::String>()) == "foo"_kj);
  KJ_EXPECT(metric.value == 1.0);
  KJ_EXPECT(metric.keyMatches("foo"_kj));

  enum class Foo { A };
  KJ_EXPECT(!metric.keyMatches(Foo::A));

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Metric>();
  metric.copyTo(builder);

  auto reader = builder.asReader();
  trace::Metric metric2(reader);
  KJ_EXPECT(metric2.type == trace::Metric::Type::COUNTER);
  KJ_EXPECT(KJ_ASSERT_NONNULL(metric2.key.tryGet<kj::String>()) == "foo"_kj);
  KJ_EXPECT(metric2.value == 1.0);

  auto metric3 = metric.clone();
  KJ_EXPECT(metric3.type == trace::Metric::Type::COUNTER);
  KJ_EXPECT(KJ_ASSERT_NONNULL(metric3.key.tryGet<kj::String>()) == "foo"_kj);
  KJ_EXPECT(metric3.value == 1.0);
}

KJ_TEST("Dropped works") {
  uint32_t a = 1;
  uint32_t b = 2;
  trace::Dropped dropped(a, b);
  KJ_EXPECT(dropped.start == a);
  KJ_EXPECT(dropped.end == b);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Dropped>();
  dropped.copyTo(builder);

  auto reader = builder.asReader();
  trace::Dropped dropped2(reader);
  KJ_EXPECT(dropped2.start == a);
  KJ_EXPECT(dropped2.end == b);

  auto dropped3 = dropped.clone();
  KJ_EXPECT(dropped3.start == a);
  KJ_EXPECT(dropped3.end == b);
}

// ======================================================================================

jsg::V8System v8System;

struct TestContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(TestContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(TestIsolate, TestContext);

static kj::Maybe<kj::StringPtr> nullNameProvider(uint32_t, trace::NameProviderContext) {
  return kj::none;
}

KJ_TEST("JS Serialization of Dropped") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::Dropped dropped(1, 2);
      auto obj = dropped.toObject(js);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser == "{\"type\":\"dropped\",\"start\":1,\"end\":2}");
    });
  });
}

KJ_TEST("JS Serialization of Mark") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::Mark mark(kj::str("foo"));
      auto obj = mark.toObject(js);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser == "{\"type\":\"mark\",\"name\":\"foo\"}");
    });
  });
}

KJ_TEST("JS Serialization of SubrequestOutcome") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::SubrequestOutcome outcome(1, kj::none, trace::SpanClose::Outcome::OK);
      auto obj = outcome.toObject(js, nullNameProvider);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser == "{\"type\":\"subrequest-outcome\",\"id\":1,\"outcome\":\"ok\"}");
    });
  });
}

KJ_TEST("JS Serialization of Subrequest") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::Subrequest subrequest(1,
          trace::Subrequest::Info(trace::FetchEventInfo(
              kj::HttpMethod::GET, kj::str("http://example.org"), kj::String(), nullptr)));
      auto obj = subrequest.toObject(js, nullNameProvider);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser ==
          "{\"type\":\"subrequest\",\"id\":1,\"info\":{\"type\":\"fetch\",\"method\":\"GET\",\"url\":\"http://example.org\",\"cfJson\":\"\"}}");
    });
  });
}

KJ_TEST("JS Serialization of Exception") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::Exception exception(
          0 * kj::MILLISECONDS + kj::UNIX_EPOCH, kj::str("foo"), kj::str("bar"), kj::none);
      auto obj = exception.toObject(js, nullNameProvider);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser ==
          "{\"type\":\"exception\",\"timestamp\":\"1970-01-01T00:00:00.000Z\",\"name\":\"foo\",\"message\":\"bar\",\"remote\":false,\"retryable\":false,\"overloaded\":false,\"durableObjectReset\":false,\"tags\":{}}");
    });
  });
}

KJ_TEST("JS Serialization of LogV2") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      jsg::Serializer ser(js);
      ser.write(js, js.num(1));
      auto data = ser.release();
      trace::LogV2 log(0 * kj::MILLISECONDS + kj::UNIX_EPOCH, LogLevel::INFO, kj::mv(data.data));
      auto obj = log.toObject(js, nullNameProvider);
      auto res = js.serializeJson(obj);
      KJ_EXPECT(res ==
          "{\"type\":\"log\",\"timestamp\":\"1970-01-01T00:00:00.000Z\",\"logLevel\":\"info\",\"message\":1,\"truncated\":false,\"tags\":{}}");
    });
  });
}

KJ_TEST("JS Serialization of DiagnosticChannelEvent") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      jsg::Serializer ser(js);
      ser.write(js, js.num(1));
      auto data = ser.release();
      trace::DiagnosticChannelEvent event(
          0 * kj::MILLISECONDS + kj::UNIX_EPOCH, kj::str("foo"), kj::mv(data.data));
      auto obj = event.toObject(js);
      auto res = js.serializeJson(obj);
      KJ_EXPECT(res ==
          "{\"type\":\"diagnostic-channel\",\"timestamp\":\"1970-01-01T00:00:00.000Z\",\"channel\":\"foo\",\"message\":1}");
    });
  });
}

KJ_TEST("JS Serialization of SpanClose") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::SpanClose event(trace::SpanClose::Outcome::OK, nullptr);
      auto obj = event.toObject(js, nullNameProvider);
      auto res = js.serializeJson(obj);
      KJ_EXPECT(res == "{\"type\":\"span\",\"outcome\":\"ok\",\"tags\":{}}");
    });
  });
}

KJ_TEST("JS Serialization of Outcome") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::FetchResponseInfo info(200);
      trace::Outcome outcome(EventOutcome::OK, kj::Maybe(kj::mv(info)));
      auto obj = outcome.toObject(js, nullNameProvider);
      auto res = js.serializeJson(obj);
      KJ_EXPECT(res ==
          "{\"type\":\"outcome\",\"outcome\":\"ok\",\"info\":{\"type\":\"fetch\",\"statusCode\":200}}");
    });
  });
}

KJ_TEST("JS Serialization of Onset") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::Onset onset(kj::str("foo"), kj::none, kj::str("bar"), kj::str("baz"), nullptr,
          kj::str("qux"), ExecutionModel::STATELESS, nullptr);

      auto obj = onset.toObject(js, nullNameProvider);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser ==
          "{\"type\":\"onset\",\"scriptName\":\"foo\",\"dispatchNamespace\":\"bar\",\"scriptId\":\"baz\",\"scriptTags\":[],\"entrypoint\":\"qux\",\"executionModel\":\"stateless\"}");
    });
  });
}

KJ_TEST("JS Serialization of HibernatableWebSocketEventInfo") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::HibernatableWebSocketEventInfo info(trace::HibernatableWebSocketEventInfo::Message{});
      auto obj = info.toObject(js);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser == "{\"type\":\"hibernatable-websocket\",\"kind\":\"message\"}");
    });
  });
}

KJ_TEST("JS Serialization of TraceEventInfo") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::TraceEventInfo info(kj::arr(trace::TraceEventInfo::TraceItem(kj::str("foo"))));
      auto obj = info.toObject(js);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser == "{\"type\":\"trace\",\"traces\":[\"foo\"]}");
    });
  });
}

KJ_TEST("JS Serialization of EmailEventInfo") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::EmailEventInfo info(kj::str("foo"), kj::str("bar"), 1);
      auto obj = info.toObject(js);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(
          ser == "{\"type\":\"email\",\"mailFrom\":\"foo\",\"rcptTo\":\"bar\",\"rawSize\":1}");
    });
  });
}

KJ_TEST("JS Serialization of QueueEventInfo") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::QueueEventInfo info(kj::str("foo"), 1);
      auto obj = info.toObject(js);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser == "{\"type\":\"queue\",\"queueName\":\"foo\",\"batchSize\":1}");
    });
  });
}

KJ_TEST("JS Serialization of AlarmEventInfo") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::AlarmEventInfo info(1 * kj::MILLISECONDS + kj::UNIX_EPOCH);
      auto obj = info.toObject(js);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser == "{\"type\":\"alarm\",\"scheduledTime\":\"1970-01-01T00:00:00.001Z\"}");
    });
  });
}

KJ_TEST("JS Serialization of ScheduledEventInfo") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::ScheduledEventInfo info(1.0, kj::String());
      auto obj = info.toObject(js);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser == "{\"type\":\"scheduled\",\"scheduledTime\":1,\"cron\":\"\"}");
    });
  });
}

KJ_TEST("JS Serialization of JsRpcEventInfo") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::JsRpcEventInfo info(kj::str("foo"));
      auto obj = info.toObject(js);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser == "{\"type\":\"jsrpc\",\"methodName\":\"foo\"}");
    });
  });
}

KJ_TEST("JS Serialization of FetchResponseInfo") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::FetchResponseInfo info(200);
      auto obj = info.toObject(js);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser == "{\"type\":\"fetch\",\"statusCode\":200}");
    });
  });
}

KJ_TEST("JS Serialization of FetchEventInfo") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      trace::FetchEventInfo info(kj::HttpMethod::GET, kj::str("http://example.org"), kj::String(),
          kj::arr(trace::FetchEventInfo::Header(kj::str("a"), kj::str("b"))));
      auto obj = info.toObject(js);
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser ==
          "{\"type\":\"fetch\",\"method\":\"GET\",\"url\":\"http://example.org\",\"cfJson\":\"\",\"headers\":{\"a\":\"b\"}}");
    });
  });
}

KJ_TEST("JS Serialization of Metrics") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<TestContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      kj::Vector<trace::Metric> metrics;
      metrics.add(trace::Metric(trace::Metric::Type::COUNTER, kj::str("foo"), 1.0));
      metrics.add(trace::Metric(trace::Metric::Type::GAUGE, (uint32_t)1, 2.0));
      auto obj = trace::Metric::toObject(js, metrics.asPtr(),
          [](uint32_t id, trace::NameProviderContext context) -> kj::Maybe<kj::StringPtr> {
        KJ_EXPECT(context == trace::NameProviderContext::METRIC);
        return "bar"_kj;
      });
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser == "{\"type\":\"metrics\",\"counters\":{\"foo\":1},\"gauges\":{\"bar\":2}}");
    });
  });
}

KJ_TEST("JS Serialization of Tags") {
  jsg::test::Evaluator<TestContext, TestIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](TestIsolate::Lock& isolateLock) {
    auto context = isolateLock.newContext<TestContext>().getHandle(isolateLock);
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock, context, [&](jsg::Lock& js) {
      kj::Vector<trace::Tag> tags;
      tags.add(trace::Tag(kj::str("foo"), true));
      tags.add(trace::Tag((uint32_t)1, kj::str("baz")));
      auto obj = trace::Tag::toObject(js, tags.asPtr(),
          [](uint32_t id, trace::NameProviderContext context) -> kj::Maybe<kj::StringPtr> {
        KJ_EXPECT(context == trace::NameProviderContext::TAG);
        return "bar"_kj;
      });
      auto ser = js.serializeJson(obj);
      KJ_EXPECT(ser == "{\"type\":\"custom\",\"tags\":{\"foo\":true,\"bar\":\"baz\"}}");

      auto obj2 = trace::Tag::toObject(js, tags.asPtr(),
          [](uint32_t id, auto) -> kj::Maybe<kj::StringPtr> { return "bar"_kj; },
          trace::Tag::ToObjectOptions::UNWRAPPED);
      auto ser2 = js.serializeJson(obj2);
      KJ_EXPECT(ser2 == "{\"foo\":true,\"bar\":\"baz\"}");
    });
  });
}

}  // namespace
}  // namespace workerd
