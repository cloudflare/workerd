// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "invocation-span.h"

#include <workerd/util/thread-scopes.h>

#include <capnp/schema.h>
#include <kj/compat/http.h>
#include <kj/encoding.h>
#include <kj/test.h>

#include <algorithm>

namespace workerd {
namespace {

using namespace tracing;

// Records the span's name and the tags it closes with.
class RecordingObserver final: public SpanObserver {
 public:
  kj::Rc<SpanObserver> newChild() override {
    return kj::rc<RecordingObserver>();
  }
  void onOpen(kj::ConstString operationName, kj::Date) override {
    name = kj::mv(operationName);
  }
  void onUpdateName(kj::ConstString operationName) override {
    name = kj::mv(operationName);
  }
  void onClose(kj::Date, Span::TagMap&& closedTags, kj::Vector<Span::Log>&&) override {
    tags = kj::mv(closedTags);
  }
  kj::Date getTime() override {
    return kj::UNIX_EPOCH;
  }

  kj::ConstString name;
  Span::TagMap tags;
};

kj::StringPtr stringTag(const Span::TagMap& tags, kj::ConstString key) {
  return KJ_ASSERT_NONNULL(tags.find(key), key).get<kj::ConstString>().asPtr();
}

int64_t intTag(const Span::TagMap& tags, kj::ConstString key) {
  return KJ_ASSERT_NONNULL(tags.find(key), key).get<int64_t>();
}

KJ_TEST("describeInvocationSpan names and tags a fetch invocation") {
  setPredictableModeForTest();
  auto observer = kj::rc<RecordingObserver>();
  auto& recorded = *observer;
  {
    SpanBuilder span(observer.addRef(), "invocation"_kjc);
    auto headers = kj::arr(FetchEventInfo::Header(kj::str("accept"), kj::str("text/html")),
        FetchEventInfo::Header(kj::str("content-length"), kj::str("12")),
        FetchEventInfo::Header(kj::str("cookie"), kj::str("secret")),
        FetchEventInfo::Header(kj::str("x-length"), kj::str("-1")),
        FetchEventInfo::Header(kj::str("user-agent"), kj::str("test-agent/1.0")));
    describeInvocationSpan(span,
        FetchEventInfo(kj::HttpMethod::POST, kj::str("https://example.com/path?q=1"),
            kj::str(R"({"asn":13335,"country":"US","city":"Austin","region":"Texas",)"
                    R"("continent":"NA","timezone":"America/Chicago","colo":"DFW"})"),
            kj::mv(headers)));
    KJ_EXPECT(kj::StringPtr(recorded.name) == "POST"_kj);
    setInvocationResponseStatus(span, 201);
    setInvocationOutcome(span, EventOutcome::OK, 1 * kj::MILLISECONDS, 2 * kj::MILLISECONDS);
  }
  auto& tags = recorded.tags;
  KJ_EXPECT(stringTag(tags, "faas.trigger"_kjc) == "http"_kj);
  KJ_EXPECT(stringTag(tags, "cloudflare.handler_type"_kjc) == "fetch"_kj);
  KJ_EXPECT(stringTag(tags, "http.request.method"_kjc) == "POST"_kj);
  KJ_EXPECT(stringTag(tags, "url.full"_kjc) == "https://example.com/path?q=1"_kj);
  KJ_EXPECT(stringTag(tags, "http.request.header.accept"_kjc) == "text/html"_kj);
  KJ_EXPECT(intTag(tags, "http.request.body.size"_kjc) == 12);
  KJ_EXPECT(stringTag(tags, "user_agent.original"_kjc) == "test-agent/1.0"_kj);
  KJ_EXPECT(tags.find("http.request.header.cookie"_kjc) == kj::none);
  KJ_EXPECT(intTag(tags, "cloudflare.asn"_kjc) == 13335);
  KJ_EXPECT(stringTag(tags, "geo.country.code"_kjc) == "US"_kj);
  KJ_EXPECT(stringTag(tags, "geo.locality.name"_kjc) == "Austin"_kj);
  KJ_EXPECT(stringTag(tags, "geo.locality.region"_kjc) == "Texas"_kj);
  KJ_EXPECT(stringTag(tags, "geo.continent.code"_kjc) == "NA"_kj);
  KJ_EXPECT(stringTag(tags, "geo.timezone"_kjc) == "America/Chicago"_kj);
  KJ_EXPECT(tags.find("cloudflare.verified_bot_category"_kjc) == kj::none);
  KJ_EXPECT(intTag(tags, "http.response.status_code"_kjc) == 201);
  KJ_EXPECT(stringTag(tags, "cloudflare.outcome"_kjc) == "ok"_kj);
  // Durations are zero in predictable mode, like the Outcome event's.
  KJ_EXPECT(intTag(tags, "cpu_time_ms"_kjc) == 0);
  KJ_EXPECT(intTag(tags, "wall_time_ms"_kjc) == 0);
}

KJ_TEST("describeInvocationSpan names and tags a jsrpc invocation") {
  setPredictableModeForTest();
  auto observer = kj::rc<RecordingObserver>();
  auto& recorded = *observer;
  {
    SpanBuilder span(observer.addRef(), "invocation"_kjc);
    describeInvocationSpan(span, JsRpcEventInfo(kj::str("placeholder")));
    KJ_EXPECT(kj::StringPtr(recorded.name) == "jsrpc"_kj);
    setInvocationRpcMethod(span, "myMethod"_kj);
    setInvocationOutcome(
        span, EventOutcome::EXCEEDED_CPU, 0 * kj::MILLISECONDS, 0 * kj::MILLISECONDS);
  }
  auto& tags = recorded.tags;
  KJ_EXPECT(stringTag(tags, "faas.trigger"_kjc) == "jsrpc"_kj);
  KJ_EXPECT(stringTag(tags, "cloudflare.handler_type"_kjc) == "jsrpc"_kj);
  KJ_EXPECT(stringTag(tags, "jsrpc.method"_kjc) == "myMethod"_kj);
  KJ_EXPECT(stringTag(tags, "cloudflare.rpcMethod"_kjc) == "myMethod"_kj);
  KJ_EXPECT(tags.find("http.request.method"_kjc) == kj::none);
  KJ_EXPECT(stringTag(tags, "cloudflare.outcome"_kjc) == "exceededCpu"_kj);
}

KJ_TEST("describeInvocationSpan names and tags a scheduled invocation") {
  setPredictableModeForTest();
  auto observer = kj::rc<RecordingObserver>();
  auto& recorded = *observer;
  {
    SpanBuilder span(observer.addRef(), "invocation"_kjc);
    describeInvocationSpan(span, ScheduledEventInfo(1709210096789.0, kj::str("*/5 * * * *")));
    KJ_EXPECT(kj::StringPtr(recorded.name) == "scheduled"_kj);
  }
  auto& tags = recorded.tags;
  KJ_EXPECT(stringTag(tags, "faas.trigger"_kjc) == "timer"_kj);
  KJ_EXPECT(stringTag(tags, "faas.cron"_kjc) == "*/5 * * * *"_kj);
  // The epoch in predictable mode, spelled as JavaScript's Date.prototype.toISOString() would.
  KJ_EXPECT(stringTag(tags, "cloudflare.scheduled_time"_kjc) == "1970-01-01T00:00:00.000Z"_kj);
}

KJ_TEST("describeInvocationSpan names and tags an onHibernate invocation") {
  setPredictableModeForTest();
  auto observer = kj::rc<RecordingObserver>();
  auto& recorded = *observer;
  {
    SpanBuilder span(observer.addRef(), "invocation"_kjc);
    describeInvocationSpan(span, OnHibernateEventInfo(OnHibernateEventInfo::Reason::INACTIVE));
    KJ_EXPECT(kj::StringPtr(recorded.name) == "onHibernate"_kj);
  }
  auto& tags = recorded.tags;
  KJ_EXPECT(stringTag(tags, "faas.trigger"_kjc) == "other"_kj);
  KJ_EXPECT(stringTag(tags, "cloudflare.handler_type"_kjc) == "onHibernate"_kj);
}

// `tags` one per line in iteration order, which is the order they are exported in, strings quoted.
kj::String describe(const Span::TagMap& tags, kj::ArrayPtr<const kj::StringPtr> skip = nullptr) {
  kj::Vector<kj::String> lines;
  for (auto& tag: tags) {
    if (std::find(skip.begin(), skip.end(), tag.key.asPtr()) != skip.end()) continue;
    KJ_SWITCH_ONEOF(tag.value) {
      KJ_CASE_ONEOF(s, kj::ConstString) {
        lines.add(kj::str(tag.key, " = \"", s, "\""));
      }
      KJ_CASE_ONEOF(b, bool) {
        lines.add(kj::str(tag.key, " = ", b));
      }
      KJ_CASE_ONEOF(i, int64_t) {
        lines.add(kj::str(tag.key, " = ", i));
      }
      KJ_CASE_ONEOF(d, double) {
        lines.add(kj::str(tag.key, " = ", d));
      }
    }
  }
  return kj::strArray(lines, "\n");
}

// The span name and tags describeInvocationSpan() gives an invocation of `info`.
kj::String describeInvocation(const EventInfo& info) {
  setPredictableModeForTest();
  auto observer = kj::rc<RecordingObserver>();
  auto& recorded = *observer;
  {
    SpanBuilder span(observer.addRef(), "invocation"_kjc);
    describeInvocationSpan(span, info);
  }
  return kj::str("name = ", recorded.name, "\n", describe(recorded.tags));
}

KJ_TEST("describeInvocationSpan names and tags every other kind of invocation") {
  KJ_EXPECT(describeInvocation(AlarmEventInfo(kj::UNIX_EPOCH + 1709210096789 * kj::MILLISECONDS)) ==
      R"(name = alarm
faas.trigger = "timer"
cloudflare.handler_type = "alarm"
cloudflare.scheduled_time = "1970-01-01T00:00:00.000Z")"_kj);
  KJ_EXPECT(describeInvocation(QueueEventInfo(kj::str("my-queue"), 12)) ==
      R"(name = queue
faas.trigger = "pubsub"
cloudflare.handler_type = "queue"
cloudflare.queue.name = "my-queue"
cloudflare.queue.batch_size = 12)"_kj);
  KJ_EXPECT(describeInvocation(
                EmailEventInfo(kj::str("from@example.com"), kj::str("to@example.com"), 4096)) ==
      R"(name = email
faas.trigger = "email"
cloudflare.handler_type = "email"
cloudflare.email.from = "from@example.com"
cloudflare.email.to = "to@example.com"
cloudflare.email.size = 4096)"_kj);
  KJ_EXPECT(describeInvocation(TraceEventInfo(kj::arr(
                TraceEventInfo::TraceItem(kj::str("a")), TraceEventInfo::TraceItem(kj::none)))) ==
      R"(name = trace
faas.trigger = "trace"
cloudflare.handler_type = "trace"
cloudflare.trace.count = 2)"_kj);
  // The kind of WebSocket event, and a close's code, are not recorded.
  constexpr auto WEBSOCKET = R"(name = hibernatableWebSocket
faas.trigger = "websocket"
cloudflare.handler_type = "hibernatableWebSocket")"_kj;
  KJ_EXPECT(describeInvocation(HibernatableWebSocketEventInfo(
                HibernatableWebSocketEventInfo::Message())) == WEBSOCKET);
  KJ_EXPECT(describeInvocation(HibernatableWebSocketEventInfo(HibernatableWebSocketEventInfo::Close{
              .code = 1011, .wasClean = false})) == WEBSOCKET);
  KJ_EXPECT(describeInvocation(HibernatableWebSocketEventInfo(
                HibernatableWebSocketEventInfo::Error())) == WEBSOCKET);
  KJ_EXPECT(describeInvocation(ConnectEventInfo()) ==
      R"(name = connect
faas.trigger = "http"
cloudflare.handler_type = "connect")"_kj);
  KJ_EXPECT(describeInvocation(CustomEventInfo()) ==
      R"(name = custom
faas.trigger = "other"
cloudflare.handler_type = "custom")"_kj);
  // The method of a JS RPC call is not known yet; see setInvocationRpcMethod().
  KJ_EXPECT(describeInvocation(JsRpcEventInfo(kj::str("method"))) ==
      R"(name = jsrpc
faas.trigger = "jsrpc"
cloudflare.handler_type = "jsrpc")"_kj);
  KJ_EXPECT(describeInvocation(OnHibernateEventInfo(OnHibernateEventInfo::Reason::INACTIVE)) ==
      R"(name = onHibernate
faas.trigger = "other"
cloudflare.handler_type = "onHibernate")"_kj);
}

KJ_TEST("describeInvocationSpan tags a fetch invocation in a fixed order") {
  auto headers = kj::arr(FetchEventInfo::Header(kj::str("accept-language"), kj::str("pt-PT")),
      FetchEventInfo::Header(kj::str("content-length"), kj::str("abc")),
      FetchEventInfo::Header(kj::str("accept-encoding"), kj::str("gzip")),
      FetchEventInfo::Header(kj::str("authorization"), kj::str("secret")),
      FetchEventInfo::Header(kj::str("accept"), kj::str("*/*")));
  KJ_EXPECT(
      describeInvocation(FetchEventInfo(kj::HttpMethod::PATCH, kj::str("http://example.com/"),
          kj::str(R"({"region":"Lisbon","asn":1,"verifiedBotCategory":"Search Engine Crawler"})"),
          kj::mv(headers))) ==
      R"(name = PATCH
faas.trigger = "http"
cloudflare.handler_type = "fetch"
http.request.method = "PATCH"
url.full = "http://example.com/"
http.request.header.accept-language = "pt-PT"
http.request.header.accept-encoding = "gzip"
http.request.header.accept = "*/*"
geo.locality.region = "Lisbon"
cloudflare.asn = 1
cloudflare.verified_bot_category = "Search Engine Crawler")"_kj);
  // A content-length is parsed as strtoll() parses it, but must be whole and not negative.
  auto bodySize = [](kj::StringPtr length) {
    auto observer = kj::rc<RecordingObserver>();
    auto& recorded = *observer;
    {
      SpanBuilder span(observer.addRef(), "invocation"_kjc);
      describeInvocationSpan(span,
          FetchEventInfo(kj::HttpMethod::GET, kj::str("http://a/"), kj::str(),
              kj::arr(FetchEventInfo::Header(kj::str("content-length"), kj::str(length)))));
    }
    // Left out is -1, which is never recorded.
    return recorded.tags.find("http.request.body.size"_kjc)
        .map([](Span::TagValue& value) {
      return value.get<int64_t>();
    }).orDefault(-1);
  };
  for (auto length: {"-1"_kj, "abc"_kj, "12 "_kj, "1.5"_kj, ""_kj, "99999999999999999999"_kj}) {
    KJ_EXPECT(bodySize(length) == -1, length);
  }
  KJ_EXPECT(bodySize("0"_kj) == 0);
  KJ_EXPECT(bodySize(" 12"_kj) == 12);
  KJ_EXPECT(bodySize("+12"_kj) == 12);
  KJ_EXPECT(bodySize("012"_kj) == 12);
  KJ_EXPECT(bodySize("0x10"_kj) == 16);
}

// The tags describeInvocationSpan() lifts from a fetch invocation's `cf` JSON.
kj::String cfTags(kj::StringPtr cfJson) {
  constexpr kj::StringPtr FETCH_TAGS[] = {
    "faas.trigger"_kj, "cloudflare.handler_type"_kj, "http.request.method"_kj, "url.full"_kj};
  setPredictableModeForTest();
  auto observer = kj::rc<RecordingObserver>();
  auto& recorded = *observer;
  {
    SpanBuilder span(observer.addRef(), "invocation"_kjc);
    describeInvocationSpan(
        span, FetchEventInfo(kj::HttpMethod::GET, kj::str("http://a/"), kj::str(cfJson), nullptr));
  }
  return describe(recorded.tags, FETCH_TAGS);
}

KJ_TEST("describeInvocationSpan ignores `cf` JSON that is not an object") {
  for (auto json: {""_kj, "null"_kj, "[]"_kj, "[{\"country\":\"PT\"}]"_kj, "\"PT\""_kj, "1"_kj,
         "{\"country\":\"PT\""_kj, "{\"country\":\"PT\",}"_kj, "{country:\"PT\"}"_kj,
         "{\"country\":\"PT\"} trailing"_kj}) {
    KJ_EXPECT(cfTags(json) == ""_kj, json, cfTags(json));
  }
}

KJ_TEST("describeInvocationSpan takes only numbers for `asn` and only strings for the rest") {
  KJ_EXPECT(cfTags(R"({"asn":"13335","country":1,"city":null,"region":{"name":"Lisbon"},)"
                   R"("timezone":["Europe/Lisbon"],"continent":true})") == ""_kj);
  // A number is truncated towards zero.
  KJ_EXPECT(cfTags(R"({"asn":13335.9})") == "cloudflare.asn = 13335"_kj);
  KJ_EXPECT(cfTags(R"({"asn":-1.9})") == "cloudflare.asn = -1"_kj);
  KJ_EXPECT(cfTags(R"({"asn":1e15})") == "cloudflare.asn = 1000000000000000"_kj);
  KJ_EXPECT(cfTags(R"({"asn":4294967296})") == "cloudflare.asn = 4294967296"_kj);
  // Only the top level, and only the exact names.
  KJ_EXPECT(cfTags(R"({"nested":{"country":"PT"},"Country":"PT","colo":"LIS"})") == ""_kj);
}

KJ_TEST("describeInvocationSpan decodes `cf` strings as they are") {
  KJ_EXPECT(cfTags(R"({"city":"São Paulo","region":"\"quoted\"\n"})") ==
      "geo.locality.name = \"São Paulo\"\ngeo.locality.region = \"\"quoted\"\n\""_kj);
  // Bytes that are not UTF-8, and an unpaired surrogate escape, are kept.
  KJ_EXPECT(cfTags("{\"city\":\"\xff\"}") == "geo.locality.name = \"\xff\""_kj,
      kj::encodeHex(cfTags("{\"city\":\"\xff\"}").asBytes()));
  auto surrogate = cfTags(R"({"city":"\ud800"})");
  KJ_EXPECT(kj::encodeHex(surrogate.asBytes()) ==
          kj::str(kj::encodeHex("geo.locality.name = \""_kj.asBytes()), "eda080", "22"),
      kj::encodeHex(surrogate.asBytes()));
}

KJ_TEST("describeInvocationSpan keeps the last of duplicate `cf` keys") {
  // Overwriting a tag is logged, as an error in predictable mode, once per process.
  KJ_EXPECT_LOG(ERROR, "overwriting previous tag");
  KJ_EXPECT(cfTags(R"({"country":"US","asn":1,"country":"PT","asn":2})") ==
      "geo.country.code = \"PT\"\ncloudflare.asn = 2"_kj);
}

KJ_TEST("setInvocationOutcome spells every outcome as the Outcome event does") {
  setPredictableModeForTest();
  kj::Vector<kj::String> spellings;
  for (auto enumerant: capnp::Schema::from<EventOutcome>().getEnumerants()) {
    auto observer = kj::rc<RecordingObserver>();
    auto& recorded = *observer;
    {
      SpanBuilder span(observer.addRef(), "invocation"_kjc);
      setInvocationOutcome(span, static_cast<EventOutcome>(enumerant.getOrdinal()),
          1 * kj::MILLISECONDS, 1 * kj::MILLISECONDS);
    }
    spellings.add(kj::str(stringTag(recorded.tags, "cloudflare.outcome"_kjc)));
  }
  KJ_EXPECT(kj::strArray(spellings, ",") ==
      "unknown,ok,exception,exceededCpu,killSwitch,daemonDown,scriptNotFound,canceled,"
      "exceededMemory,loadShed,responseStreamDisconnected,internalError,exceededWallTime,"
      "aborted"_kj);
}

KJ_TEST("describeInvocationSpan leaves an unobserved span alone") {
  SpanBuilder span(nullptr);
  describeInvocationSpan(span, CustomEventInfo());
  setInvocationResponseStatus(span, 200);
  setInvocationRpcMethod(span, "method"_kj);
  setInvocationOutcome(span, EventOutcome::OK, 0 * kj::MILLISECONDS, 0 * kj::MILLISECONDS);
}

}  // namespace
}  // namespace workerd
