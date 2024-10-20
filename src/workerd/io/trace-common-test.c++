#include "trace-common.h"

#include <capnp/message.h>
#include <kj/compat/http.h>
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("Onset works") {
  trace::Onset onset(kj::str("bar"), kj::none, kj::str("baz"), kj::str("qux"),
      kj::arr(kj::str("quux")), kj::str("corge"), ExecutionModel::STATELESS);

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
  auto data = kj::heapArray<kj::byte>(1);
  trace::LogV2 log(LogLevel::INFO, kj::heapArray<kj::byte>(data));
  KJ_EXPECT(log.logLevel == LogLevel::INFO);
  KJ_EXPECT(log.message == data);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::LogV2>();
  log.copyTo(builder);

  auto reader = builder.asReader();
  trace::LogV2 log2(reader);
  KJ_EXPECT(log2.logLevel == LogLevel::INFO);
  KJ_EXPECT(log2.message == log.message);

  auto log3 = log.clone();
  KJ_EXPECT(log3.logLevel == LogLevel::INFO);
  KJ_EXPECT(log3.message == log.message);
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
  trace::Subrequest subrequest(trace::Subrequest::Info(trace::FetchEventInfo(
      kj::HttpMethod::GET, kj::str("http://example.org"), kj::String(), nullptr)));
  KJ_EXPECT(KJ_ASSERT_NONNULL(subrequest.info).is<trace::FetchEventInfo>());

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Subrequest>();
  subrequest.copyTo(builder);

  auto reader = builder.asReader();
  trace::Subrequest subrequest2(reader);
  KJ_EXPECT(KJ_ASSERT_NONNULL(subrequest.info).is<trace::FetchEventInfo>());

  auto subrequest3 = subrequest.clone();
  KJ_EXPECT(KJ_ASSERT_NONNULL(subrequest.info).is<trace::FetchEventInfo>());
}

KJ_TEST("SpanClose works") {
  trace::SpanClose event(EventOutcome::OK, kj::none);
  KJ_EXPECT(event.outcome == EventOutcome::OK);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::SpanClose>();
  event.copyTo(builder);

  auto reader = builder.asReader();
  trace::SpanClose event2(reader);
  KJ_EXPECT(event2.outcome == EventOutcome::OK);
  KJ_EXPECT(event2.info == kj::none);

  auto event3 = event.clone();
  KJ_EXPECT(event3.outcome == EventOutcome::OK);
  KJ_EXPECT(event3.info == kj::none);
}

KJ_TEST("Metric works") {
  trace::Metric metric(trace::Metric::Type::COUNTER, "foo", 1.0);
  KJ_EXPECT(metric.type == trace::Metric::Type::COUNTER);
  KJ_EXPECT(metric.key == "foo"_kj);
  KJ_EXPECT(metric.value == 1.0);

  capnp::MallocMessageBuilder message;
  auto builder = message.initRoot<rpc::Trace::Metric>();
  metric.copyTo(builder);

  auto reader = builder.asReader();
  trace::Metric metric2(reader);
  KJ_EXPECT(metric2.type == trace::Metric::Type::COUNTER);
  KJ_EXPECT(metric2.key == metric.key);
  KJ_EXPECT(metric2.value == 1.0);

  auto metric3 = metric.clone();
  KJ_EXPECT(metric3.type == trace::Metric::Type::COUNTER);
  KJ_EXPECT(metric3.key == metric.key);
  KJ_EXPECT(metric3.value == 1.0);
}

}  // namespace
}  // namespace workerd
