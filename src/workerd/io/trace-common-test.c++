#include "trace-common.h"

#include <capnp/compat/json.h>
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
  uint32_t a = 1;
  auto tags = kj::arr(trace::Tag(kj::str("a"), static_cast<uint64_t>(1)));
  trace::Onset onset(a, kj::str("foo"), kj::str("bar"), kj::none, kj::str("baz"), kj::str("qux"),
      kj::arr(kj::str("quux")), kj::str("corge"), kj::mv(tags));

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Onset>();
  onset.copyTo(builder);

  auto reader = builder.asReader();
  trace::Onset onset2(reader);
  KJ_EXPECT(onset.accountId == a);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset.stableId) == "foo"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset.scriptName) == "bar"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset.dispatchNamespace) == "baz"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset.scriptId) == "qux"_kj);
  KJ_EXPECT(onset.scriptTags.size() == 1);
  KJ_EXPECT(onset.scriptTags[0] == "quux"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset.entrypoint) == "corge"_kj);
  KJ_EXPECT(onset.tags.size() == 1);
  KJ_EXPECT(onset.tags[0].keyMatches("a"_kj));

  capnp::JsonCodec json;
  json.setPrettyPrint(true);
  auto encoded = json.encode(builder);
  auto check = R"({ "accountId": 1,
  "stableId": "foo",
  "scriptName": "bar",
  "dispatchNamespace": "baz",
  "scriptId": "qux",
  "scriptTags": ["quux"],
  "entrypoint": "corge",
  "tags": [{"key": {"text": "a"}, "value": {"uint64": "1"}}] })";

  KJ_EXPECT(encoded == check);

  auto onset3 = onset.clone();
  KJ_EXPECT(onset3.accountId == a);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset3.stableId) == "foo"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset3.scriptName) == "bar"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset3.dispatchNamespace) == "baz"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset3.scriptId) == "qux"_kj);
  KJ_EXPECT(onset3.scriptTags.size() == 1);
  KJ_EXPECT(onset3.scriptTags[0] == "quux"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(onset3.entrypoint) == "corge"_kj);
  KJ_EXPECT(onset3.tags.size() == 1);
  KJ_EXPECT(onset3.tags[0].keyMatches("a"_kj));
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

KJ_TEST("ActorFlushInfo works") {
  auto tags =
      kj::arr(trace::Tag(trace::ActorFlushInfo::CommonTags::REASON, static_cast<uint64_t>(1)),
          trace::Tag(trace::ActorFlushInfo::CommonTags::BROKEN, true));
  trace::ActorFlushInfo info(kj::mv(tags));

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::ActorFlushInfo>();
  info.copyTo(builder);

  auto reader = builder.asReader();
  trace::ActorFlushInfo info2(reader);
  KJ_EXPECT(info2.tags.size() == 2);
  KJ_EXPECT(info2.tags[0].keyMatches(trace::ActorFlushInfo::CommonTags::REASON));
  KJ_EXPECT(info2.tags[1].keyMatches(trace::ActorFlushInfo::CommonTags::BROKEN));

  capnp::JsonCodec json;
  json.setPrettyPrint(true);
  auto encoded = json.encode(builder);
  KJ_EXPECT(encoded ==
      R"({"tags": [{"key": {"id": 0}, "value": {"uint64": "1"}}, {"key": {"id": 1}, "value": {"bool": true}}]})");

  auto info3 = info.clone();
  KJ_EXPECT(info3.tags.size() == 2);
  KJ_EXPECT(info3.tags[0].keyMatches(trace::ActorFlushInfo::CommonTags::REASON));
  KJ_EXPECT(info3.tags[1].keyMatches(trace::ActorFlushInfo::CommonTags::BROKEN));
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
  trace::Outcome outcome(EventOutcome::OK);
  KJ_EXPECT(outcome.outcome == EventOutcome::OK);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Outcome>();
  outcome.copyTo(builder);

  auto reader = builder.asReader();
  trace::Outcome outcome2(reader);
  KJ_EXPECT(outcome2.outcome == EventOutcome::OK);

  auto outcome3 = outcome.clone();
  KJ_EXPECT(outcome3.outcome == EventOutcome::OK);
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
  trace::SubrequestOutcome outcome(a, kj::none, trace::Span::Outcome::OK);
  KJ_EXPECT(outcome.id == a);
  KJ_EXPECT(outcome.outcome == trace::Span::Outcome::OK);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::SubrequestOutcome>();
  outcome.copyTo(builder);

  auto reader = builder.asReader();
  trace::SubrequestOutcome outcome2(reader);
  KJ_EXPECT(outcome2.id == a);
  KJ_EXPECT(outcome2.outcome == trace::Span::Outcome::OK);

  auto outcome3 = outcome.clone();
  KJ_EXPECT(outcome3.id == a);
  KJ_EXPECT(outcome3.outcome == trace::Span::Outcome::OK);
}

KJ_TEST("Span works") {
  uint32_t a = 1;
  uint32_t b = 0;
  trace::Span event(a, b, trace::Span::Outcome::OK, false, kj::none, nullptr);
  KJ_EXPECT(event.id == a);
  KJ_EXPECT(event.parent == b);
  KJ_EXPECT(event.outcome == trace::Span::Outcome::OK);
  KJ_EXPECT(event.transactional == false);
  KJ_EXPECT(event.info == kj::none);
  KJ_EXPECT(event.tags.size() == 0);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Span>();
  event.copyTo(builder);

  auto reader = builder.asReader();
  trace::Span event2(reader);
  KJ_EXPECT(event2.id == a);
  KJ_EXPECT(event2.parent == b);
  KJ_EXPECT(event2.outcome == trace::Span::Outcome::OK);
  KJ_EXPECT(event2.transactional == false);
  KJ_EXPECT(event2.info == kj::none);
  KJ_EXPECT(event2.tags.size() == 0);

  auto event3 = event.clone();
  KJ_EXPECT(event3.id == a);
  KJ_EXPECT(event3.parent == b);
  KJ_EXPECT(event3.outcome == trace::Span::Outcome::OK);
  KJ_EXPECT(event3.transactional == false);
  KJ_EXPECT(event3.info == kj::none);
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
  KJ_EXPECT(KJ_ASSERT_NONNULL(metric.value.tryGet<double>()) == 1.0);
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
  KJ_EXPECT(KJ_ASSERT_NONNULL(metric2.value.tryGet<double>()) == 1.0);

  auto metric3 = metric.clone();
  KJ_EXPECT(metric3.type == trace::Metric::Type::COUNTER);
  KJ_EXPECT(KJ_ASSERT_NONNULL(metric3.key.tryGet<kj::String>()) == "foo"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(metric3.value.tryGet<double>()) == 1.0);
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

}  // namespace
}  // namespace workerd
