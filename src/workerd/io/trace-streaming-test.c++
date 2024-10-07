#include "trace-streaming.h"

#include <kj/compat/http.h>
#include <kj/test.h>

namespace workerd {
namespace {

static auto idFactory = StreamingTrace::IdFactory::newUuidIdFactory();

struct MockTimeProvider final: public StreamingTrace::TimeProvider {
  kj::Date getNow() const override {
    return 0 * kj::MILLISECONDS + kj::UNIX_EPOCH;
  }
};
MockTimeProvider mockTimeProvider;

KJ_TEST("We can create a simple, empty streaming trace session with implicit unknown outcome") {
  trace::Onset onset;
  // In this test we are creating a simple trace with no events or spans.
  // The delegate should be called exactly twice, once with the onset and
  // once with an implicit unknown outcome (since we're not explicitly calling
  // setOutcome ourselves)
  int callCount = 0;
  kj::String id = nullptr;
  {
    auto streamingTrace = workerd::StreamingTrace::create(
        *idFactory, kj::mv(onset), [&callCount, &id](workerd::StreamEvent&& event) {
      KJ_EXPECT(event.span.id == 0, "the root span should have id 0");
      KJ_EXPECT(event.span.parent == 0, "the root span should have no parent");
      KJ_EXPECT(event.span.transactional == false, "the root span should not be transactional");
      switch (callCount++) {
        case 0: {
          id = kj::str(event.id);
          KJ_EXPECT(id.size() > 0, "there should be a non-empty id. we don't care what it is.");
          KJ_EXPECT(event.sequence == 0, "the sequence should be 0");
          KJ_ASSERT_NONNULL(
              event.event.tryGet<trace::Onset>(), "the event should be an onset event");
          break;
        }
        case 1: {
          KJ_EXPECT(event.id == id, "the event id should be the same as the onset id");
          KJ_EXPECT(event.sequence == 1, "the sequence should have been incremented");
          auto& outcome = KJ_ASSERT_NONNULL(
              event.event.tryGet<trace::Outcome>(), "the event should be an outcome event");
          KJ_EXPECT(outcome.outcome == EventOutcome::UNKNOWN, "the outcome should be unknown");
          break;
        }
      }
    }, mockTimeProvider);
  }
  KJ_EXPECT(callCount == 2);
}

KJ_TEST("We can create a simple, empty streaming trace session with expicit canceled outcome") {
  trace::Onset onset;
  // In this test we are creating a simple trace with no events or spans.
  // The delegate should be called exactly twice, once with the onset and
  // once with an explicit canceled outcome.
  int callCount = 0;
  kj::String id = nullptr;
  auto streamingTrace = workerd::StreamingTrace::create(
      *idFactory, kj::mv(onset), [&callCount, &id](workerd::StreamEvent&& event) {
    KJ_EXPECT(event.span.id == 0, "the root span should have id 0");
    KJ_EXPECT(event.span.parent == 0, "the root span should have no parent");
    KJ_EXPECT(event.span.transactional == false, "the root span should not be transactional");
    switch (callCount++) {
      case 0: {
        id = kj::str(event.id);
        KJ_EXPECT(id.size() > 0, "there should be a non-empty id. we don't care what it is.");
        KJ_EXPECT(event.sequence == 0, "the sequence should be 0");
        KJ_ASSERT_NONNULL(event.event.tryGet<trace::Onset>(), "the event should be an onset event");
        break;
      }
      case 1: {
        KJ_EXPECT(event.id == id, "the event id should be the same as the onset id");
        KJ_EXPECT(event.sequence == 1, "the sequence should have been incremented");
        auto& outcome = KJ_ASSERT_NONNULL(
            event.event.tryGet<trace::Outcome>(), "the event should be an outcome event");
        KJ_EXPECT(outcome.outcome == EventOutcome::CANCELED, "the outcome should be canceled");
        break;
      }
    }
  }, mockTimeProvider);
  streamingTrace->setOutcome(trace::Outcome(EventOutcome::CANCELED));
  KJ_EXPECT(callCount == 2);
}

KJ_TEST(
    "We can create a simple, streaming trace session with a single implicitly unknown stage span") {
  trace::Onset onset;
  // In this test we are creating a simple trace with no events or spans.
  // The delegate should be called exactly four times.
  int callCount = 0;
  kj::String id = nullptr;
  {
    auto streamingTrace = workerd::StreamingTrace::create(
        *idFactory, kj::mv(onset), [&callCount, &id](workerd::StreamEvent&& event) {
      switch (callCount++) {
        case 0: {
          KJ_EXPECT(event.span.id == 0, "the root span should have id 0");
          KJ_EXPECT(event.span.parent == 0, "the root span should have no parent");
          KJ_EXPECT(event.span.transactional == false, "the root span should not be transactional");
          id = kj::str(event.id);
          KJ_EXPECT(id.size() > 0, "there should be a non-empty id. we don't care what it is.");
          KJ_EXPECT(event.sequence == 0, "the sequence should be 0");
          KJ_ASSERT_NONNULL(
              event.event.tryGet<trace::Onset>(), "the event should be an onset event");
          break;
        }
        case 1: {
          KJ_EXPECT(event.span.id == 1, "the stage span should have id 1");
          KJ_EXPECT(event.span.parent == 0, "the parent span should be the root span");
          KJ_EXPECT(event.span.transactional == false, "the root span should not be transactional");
          KJ_EXPECT(event.id == id);
          auto& detail = KJ_ASSERT_NONNULL(event.event.tryGet<StreamEvent::Info>());
          auto& fetch = KJ_ASSERT_NONNULL(
              detail.tryGet<trace::FetchEventInfo>(), "the event should be a fetch event");
          KJ_EXPECT(fetch.method == kj::HttpMethod::GET, "the method should be GET");
          break;
        }
        case 2: {
          KJ_EXPECT(event.span.id == 1, "the stage span should have id 1");
          KJ_EXPECT(event.span.parent == 0, "the parent span should be the root span");
          KJ_EXPECT(event.span.transactional == false, "the root span should not be transactional");
          KJ_EXPECT(event.id == id);
          auto& span = KJ_ASSERT_NONNULL(
              event.event.tryGet<trace::Span>(), "the event should be a span event");
          KJ_EXPECT(span.outcome == rpc::Trace::Span::SpanOutcome::UNKNOWN);
          break;
        }
        case 3: {
          KJ_EXPECT(event.span.id == 0, "the root span should have id 0");
          KJ_EXPECT(event.span.parent == 0, "the root span should have no parent");
          KJ_EXPECT(event.span.transactional == false, "the root span should not be transactional");
          KJ_EXPECT(event.id == id, "the event id should be the same as the onset id");
          KJ_EXPECT(event.sequence == 3, "the sequence should have been incremented");
          auto& outcome = KJ_ASSERT_NONNULL(
              event.event.tryGet<trace::Outcome>(), "the event should be an outcome event");
          KJ_EXPECT(outcome.outcome == EventOutcome::UNKNOWN, "the outcome should be canceled");
          break;
        }
      }
    }, mockTimeProvider);
    auto stage = KJ_ASSERT_NONNULL(streamingTrace->newStageSpan());
    stage->setEventInfo(0 * kj::MILLISECONDS + kj::UNIX_EPOCH,
        trace::FetchEventInfo(
            kj::HttpMethod::GET, kj::str("http://example.com"), kj::String(), {}));
    // Intentionally not calling setOutcome on the stage span or the trace itself.
  }
  KJ_EXPECT(callCount == 4);
}

KJ_TEST("We can create a simple, streaming trace session with a single explicitly canceled trace") {
  trace::Onset onset;
  // In this test we are creating a simple trace with no events or spans.
  // The delegate should be called exactly five times.
  int callCount = 0;
  kj::String id = nullptr;
  {
    auto streamingTrace = workerd::StreamingTrace::create(
        *idFactory, kj::mv(onset), [&callCount, &id](workerd::StreamEvent&& event) {
      switch (callCount++) {
        case 0: {
          KJ_EXPECT(event.span.id == 0, "the root span should have id 0");
          KJ_EXPECT(event.span.parent == 0, "the root span should have no parent");
          KJ_EXPECT(event.span.transactional == false, "the root span should not be transactional");
          id = kj::str(event.id);
          KJ_EXPECT(id.size() > 0, "there should be a non-empty id. we don't care what it is.");
          KJ_EXPECT(event.sequence == 0, "the sequence should be 0");
          KJ_ASSERT_NONNULL(
              event.event.tryGet<trace::Onset>(), "the event should be an onset event");
          break;
        }
        case 1: {
          KJ_EXPECT(event.span.id == 1, "the stage span should have id 1");
          KJ_EXPECT(event.span.parent == 0, "the parent span should be the root span");
          KJ_EXPECT(event.span.transactional == false, "the root span should not be transactional");
          KJ_EXPECT(event.id == id);
          KJ_EXPECT(event.sequence == 1);
          auto& detail = KJ_ASSERT_NONNULL(event.event.tryGet<StreamEvent::Info>());
          auto& fetch = KJ_ASSERT_NONNULL(
              detail.tryGet<trace::FetchEventInfo>(), "the event should be a fetch event");
          KJ_EXPECT(fetch.method == kj::HttpMethod::GET, "the method should be GET");
          break;
        }
        case 2: {
          KJ_EXPECT(event.span.id == 1, "the stage span should have id 1");
          KJ_EXPECT(event.span.parent == 0, "the parent span should be the root span");
          KJ_EXPECT(event.span.transactional == false, "the root span should not be transactional");
          KJ_EXPECT(event.id == id);
          KJ_EXPECT(event.sequence == 2);
          auto& detail = KJ_ASSERT_NONNULL(event.event.tryGet<StreamEvent::Detail>());
          auto& mark = KJ_ASSERT_NONNULL(detail.tryGet<trace::Mark>());
          KJ_EXPECT(mark.name == "bar"_kj);
          break;
        }
        case 3: {
          KJ_EXPECT(event.span.id == 1, "the stage span should have id 1");
          KJ_EXPECT(event.span.parent == 0, "the parent span should be the root span");
          KJ_EXPECT(event.span.transactional == false, "the root span should not be transactional");
          KJ_EXPECT(event.sequence, 3);
          KJ_EXPECT(event.id == id);
          auto& span = KJ_ASSERT_NONNULL(
              event.event.tryGet<trace::Span>(), "the event should be a span event");
          KJ_EXPECT(span.outcome == rpc::Trace::Span::SpanOutcome::CANCELED);
          break;
        }
        case 4: {
          KJ_EXPECT(event.span.id == 0, "the root span should have id 0");
          KJ_EXPECT(event.span.parent == 0, "the root span should have no parent");
          KJ_EXPECT(event.span.transactional == false, "the root span should not be transactional");
          KJ_EXPECT(event.id == id, "the event id should be the same as the onset id");
          KJ_EXPECT(event.sequence == 4, "the sequence should have been incremented");
          auto& outcome = KJ_ASSERT_NONNULL(
              event.event.tryGet<trace::Outcome>(), "the event should be an outcome event");
          KJ_EXPECT(outcome.outcome == EventOutcome::CANCELED, "the outcome should be canceled");
          break;
        }
      }
    }, mockTimeProvider);
    auto stage = KJ_ASSERT_NONNULL(streamingTrace->newStageSpan());
    stage->setEventInfo(0 * kj::MILLISECONDS + kj::UNIX_EPOCH,
        trace::FetchEventInfo(
            kj::HttpMethod::GET, kj::str("http://example.com"), kj::String(), {}));
    stage->addMark(trace::Mark(kj::str("bar")));
    // Intentionally not calling setOutcome on the stage span or the trace itself.
    streamingTrace->setOutcome(trace::Outcome(EventOutcome::CANCELED));

    // Once the outcome is set, no more events should be emitted but calling the methods on
    // the span shouldn't crash or error.
    stage->addMark(trace::Mark(kj::str("foo")));
  }
  KJ_EXPECT(callCount == 5);
}

}  // namespace
}  // namespace workerd
