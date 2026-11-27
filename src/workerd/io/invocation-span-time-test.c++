// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The times and durations an invocation span records outside predictable mode, which replaces
// them with zero. invocation-span-test.c++ covers the rest in predictable mode, which cannot be
// left once entered, so these tests run in a binary of their own.

#include "invocation-span.h"

#include <workerd/util/thread-scopes.h>

#include <kj/test.h>

namespace workerd {
namespace {

using namespace tracing;

class RecordingObserver final: public SpanObserver {
 public:
  kj::Rc<SpanObserver> newChild() override {
    return kj::rc<RecordingObserver>();
  }
  void onOpen(kj::ConstString, kj::Date) override {}
  void onClose(kj::Date, Span::TagMap&& closedTags, kj::Vector<Span::Log>&&) override {
    tags = kj::mv(closedTags);
  }
  kj::Date getTime() override {
    return kj::UNIX_EPOCH;
  }

  Span::TagMap tags;
};

// The tags an invocation of `info` is described with.
Span::TagMap describe(const EventInfo& info) {
  KJ_ASSERT(!isPredictableModeForTest());
  auto observer = kj::rc<RecordingObserver>();
  auto& recorded = *observer;
  {
    SpanBuilder span(observer.addRef(), "invocation"_kjc);
    describeInvocationSpan(span, info);
  }
  return kj::mv(recorded.tags);
}

kj::StringPtr scheduledTime(const Span::TagMap& tags) {
  return KJ_ASSERT_NONNULL(tags.find("cloudflare.scheduled_time"_kjc))
      .get<kj::ConstString>()
      .asPtr();
}

// A scheduled event's time is JavaScript's milliseconds since the epoch.
kj::String scheduledAt(double millis) {
  return kj::str(scheduledTime(describe(ScheduledEventInfo(millis, kj::str("* * * * *")))));
}

kj::String alarmAt(kj::Date date) {
  return kj::str(scheduledTime(describe(AlarmEventInfo(date))));
}

KJ_TEST("scheduled times are spelled as Date.prototype.toISOString() spells them") {
  KJ_EXPECT(scheduledAt(0) == "1970-01-01T00:00:00.000Z");
  KJ_EXPECT(scheduledAt(1709210096789.0) == "2024-02-29T12:34:56.789Z");
  // Fractions of a millisecond are cut off.
  KJ_EXPECT(scheduledAt(1709210096789.9) == "2024-02-29T12:34:56.789Z");
  // The last millisecond whose nanoseconds since the epoch fit in 64 bits, as kj::Date counts.
  KJ_EXPECT(scheduledAt(9223372036854.0) == "2262-04-11T23:47:16.854Z");
  KJ_EXPECT(alarmAt(kj::UNIX_EPOCH) == "1970-01-01T00:00:00.000Z");
  KJ_EXPECT(alarmAt(kj::UNIX_EPOCH + 1709210096789 * kj::MILLISECONDS + 999 * kj::MICROSECONDS) ==
      "2024-02-29T12:34:56.789Z");
}

KJ_TEST("scheduled times before the epoch are not spelled as toISOString() spells them") {
  // toISOString() gives 1969-12-31T23:59:59.999Z and 1969-12-31T23:59:58.500Z. Here the seconds
  // are rounded towards zero, and the milliseconds left over are printed with their sign.
  KJ_EXPECT(scheduledAt(-1) == "1970-01-01T00:00:00.-01Z", scheduledAt(-1));
  KJ_EXPECT(scheduledAt(-1500) == "1969-12-31T23:59:59.-500Z", scheduledAt(-1500));
  KJ_EXPECT(alarmAt(kj::UNIX_EPOCH - 1 * kj::MILLISECONDS) == "1970-01-01T00:00:00.-01Z");
  // Whole seconds come out right.
  KJ_EXPECT(scheduledAt(-1000) == "1969-12-31T23:59:59.000Z");
  KJ_EXPECT(alarmAt(kj::UNIX_EPOCH - 86400 * kj::SECONDS) == "1969-12-31T00:00:00.000Z");
}

KJ_TEST("setInvocationOutcome records durations in whole milliseconds") {
  KJ_ASSERT(!isPredictableModeForTest());
  auto observer = kj::rc<RecordingObserver>();
  auto& recorded = *observer;
  {
    SpanBuilder span(observer.addRef(), "invocation"_kjc);
    setInvocationOutcome(span, EventOutcome::OK, 1999 * kj::MICROSECONDS, 5 * kj::SECONDS);
  }
  auto millis = [&](kj::StringPtr key) {
    return KJ_ASSERT_NONNULL(recorded.tags.find(kj::ConstString(kj::str(key)))).get<int64_t>();
  };
  KJ_EXPECT(millis("cpu_time_ms") == 1);
  KJ_EXPECT(millis("wall_time_ms") == 5000);
}

}  // namespace
}  // namespace workerd
