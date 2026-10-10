// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "invocation-span.h"

#include <workerd/util/thread-scopes.h>

#include <kj/compat/http.h>
#include <kj/test.h>

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

}  // namespace
}  // namespace workerd
