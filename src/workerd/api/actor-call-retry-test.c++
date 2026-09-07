// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-call-retry.h"

#include <workerd/jsg/util.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

class TestTimerChannel final: public TimerChannel {
 public:
  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date>) override {
    return kj::UNIX_EPOCH;
  }

  kj::Promise<void> atTime(kj::Date) override {
    return kj::NEVER_DONE;
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration) override {
    return kj::NEVER_DONE;
  }

  kj::TimePoint nowForLimitTimeout() override {
    return currentTime;
  }

  void advance(kj::Duration duration) {
    currentTime += duration;
  }

 private:
  kj::TimePoint currentTime = kj::origin<kj::TimePoint>();
};

class RecordingObserver final: public RequestObserver {
 public:
  void recordActorRetry(ActorRetryCallType callType) override {
    retryCallTypes.add(callType);
  }

  void recordActorRetryOutcome(
      ActorRetryCallType callType, ActorRetryOutcome outcome, kj::Duration) override {
    outcomeCallTypes.add(callType);
    outcomes.add(outcome);
  }

  kj::Vector<ActorRetryCallType> retryCallTypes;
  kj::Vector<ActorRetryCallType> outcomeCallTypes;
  kj::Vector<ActorRetryOutcome> outcomes;
};

kj::Rc<ActorCallRetryState> newRetryState(TestTimerChannel& timer, RecordingObserver& observer) {
  return kj::rc<ActorCallRetryState>(timer, observer,
      ActorCallRetryState::Config{
        .callType = ActorRetryCallType::JSRPC,
        .observationEnabled = ActorRetryGateEnabled::YES,
        .enforcementEnabled = ActorRetryGateEnabled::YES,
        .payloadReplayable = ActorCallPayloadReplayable::YES,
      });
}

ActorCallRetryState::Attempt startAttempt(ActorCallRetryState& state) {
  auto result = state.startAttempt();
  return kj::mv(KJ_REQUIRE_NONNULL(result.tryGet<ActorCallRetryState::Attempt>()));
}

kj::Exception makeDisconnect(kj::StringPtr description) {
  return KJ_EXCEPTION(DISCONNECTED, description);
}

kj::OneOf<kj::Duration, kj::Exception> handleFailure(
    ActorCallRetryState& state, kj::Exception exception) {
  return state.handleAttemptFailure(kj::mv(exception));
}

KJ_TEST("not-delivered actor calls retry with a new token") {
  TestTimerChannel timer;
  auto observer = kj::refcounted<RecordingObserver>();
  auto state = newRetryState(timer, *observer);

  auto first = startAttempt(*state);
  auto firstNonce = KJ_ASSERT_NONNULL(first.getMetadata()).nonce;
  KJ_EXPECT(KJ_ASSERT_NONNULL(first.getMetadata()).isRetry == IsActorRetry::NO);

  auto notDelivered = makeDisconnect("not delivered"_kj);
  jsg::markActorRequestNotDelivered(notDelivered);
  KJ_EXPECT(handleFailure(*state, kj::mv(notDelivered)).is<kj::Duration>());

  auto second = startAttempt(*state);
  auto secondNonce = KJ_ASSERT_NONNULL(second.getMetadata()).nonce;
  KJ_EXPECT(secondNonce != firstNonce);
  KJ_EXPECT(KJ_ASSERT_NONNULL(second.getMetadata()).isRetry == IsActorRetry::NO);

  state->recordRecovered();
}

KJ_TEST("ambiguous actor calls retry with the same token") {
  TestTimerChannel timer;
  auto observer = kj::refcounted<RecordingObserver>();
  auto state = newRetryState(timer, *observer);

  auto first = startAttempt(*state);
  auto firstNonce = KJ_ASSERT_NONNULL(first.getMetadata()).nonce;

  KJ_EXPECT(handleFailure(*state, makeDisconnect("ambiguous"_kj)).is<kj::Duration>());

  auto second = startAttempt(*state);
  KJ_EXPECT(KJ_ASSERT_NONNULL(second.getMetadata()).nonce == firstNonce);
  KJ_EXPECT(KJ_ASSERT_NONNULL(second.getMetadata()).isRetry == IsActorRetry::YES);

  state->recordRecovered();
}

KJ_TEST("actor retries count only the first attempt as a subrequest") {
  TestTimerChannel timer;
  auto observer = kj::refcounted<RecordingObserver>();
  auto state = newRetryState(timer, *observer);

  auto first = startAttempt(*state);
  KJ_EXPECT(first.getCountSubrequest() == CountSubrequest::YES);

  KJ_EXPECT(handleFailure(*state, makeDisconnect("ambiguous"_kj)).is<kj::Duration>());

  auto second = startAttempt(*state);
  KJ_EXPECT(second.getCountSubrequest() == CountSubrequest::NO);

  state->recordRecovered();
}

KJ_TEST("successful actor retries report recovered") {
  TestTimerChannel timer;
  auto observer = kj::refcounted<RecordingObserver>();
  auto state = newRetryState(timer, *observer);

  startAttempt(*state);
  KJ_EXPECT(handleFailure(*state, makeDisconnect("ambiguous"_kj)).is<kj::Duration>());
  startAttempt(*state);

  KJ_ASSERT(observer->retryCallTypes.size() == 1);
  KJ_EXPECT(observer->retryCallTypes[0] == ActorRetryCallType::JSRPC);

  state->recordRecovered();
  KJ_ASSERT(observer->outcomes.size() == 1);
  KJ_EXPECT(observer->outcomeCallTypes[0] == ActorRetryCallType::JSRPC);
  KJ_EXPECT(observer->outcomes[0] == ActorRetryOutcome::RECOVERED);
}

KJ_TEST("actor retries return the original disconnect after claim rejection") {
  TestTimerChannel timer;
  auto observer = kj::refcounted<RecordingObserver>();
  auto state = newRetryState(timer, *observer);

  startAttempt(*state);
  auto original = makeDisconnect("original disconnect"_kj);
  KJ_EXPECT(handleFailure(*state, kj::mv(original)).is<kj::Duration>());
  startAttempt(*state);

  auto rejected = KJ_EXCEPTION(FAILED, "claim rejected");
  rejected.setDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID, kj::heapArray<kj::byte>(0));
  auto result = handleFailure(*state, kj::mv(rejected));
  auto& failure = KJ_ASSERT_NONNULL(result.tryGet<kj::Exception>());
  KJ_EXPECT(failure.getType() == kj::Exception::Type::DISCONNECTED);
  KJ_EXPECT(failure.getDescription().contains("original disconnect"));
  KJ_ASSERT(observer->outcomes.size() == 1);
  KJ_EXPECT(observer->outcomes[0] == ActorRetryOutcome::CLAIM_REJECTED);
}

KJ_TEST("actor retries stop after five total attempts") {
  TestTimerChannel timer;
  auto observer = kj::refcounted<RecordingObserver>();
  auto state = newRetryState(timer, *observer);

  for (uint attempt = 0; attempt < 5; ++attempt) {
    startAttempt(*state);
    auto exception = makeDisconnect("disconnected"_kj);
    auto result = handleFailure(*state, kj::mv(exception));
    if (attempt < 4) {
      KJ_EXPECT(result.is<kj::Duration>());
    } else {
      KJ_EXPECT(result.is<kj::Exception>());
    }
  }

  KJ_ASSERT(observer->retryCallTypes.size() == 4);
  KJ_ASSERT(observer->outcomes.size() == 1);
  KJ_EXPECT(observer->outcomes[0] == ActorRetryOutcome::RETRIES_EXHAUSTED);
}

KJ_TEST("actor retries return the original disconnect when the deadline expires before retry") {
  TestTimerChannel timer;
  auto observer = kj::refcounted<RecordingObserver>();
  auto state = newRetryState(timer, *observer);

  startAttempt(*state);
  auto original = makeDisconnect("original disconnect"_kj);
  KJ_EXPECT(handleFailure(*state, kj::mv(original)).is<kj::Duration>());
  timer.advance(11 * kj::SECONDS);

  auto result = state->startAttempt();
  auto& failure = KJ_ASSERT_NONNULL(result.tryGet<kj::Exception>());
  KJ_EXPECT(failure.getDescription().contains("original disconnect"));
  KJ_EXPECT(observer->retryCallTypes.size() == 0);
  KJ_EXPECT(observer->outcomes.size() == 0);
}

KJ_TEST("actor calls do not retry a failure after the deadline") {
  TestTimerChannel timer;
  auto observer = kj::refcounted<RecordingObserver>();
  auto state = newRetryState(timer, *observer);

  startAttempt(*state);
  timer.advance(11 * kj::SECONDS);
  auto failure = makeDisconnect("first attempt exceeded deadline"_kj);
  jsg::markActorRequestNotDelivered(failure);

  auto result = handleFailure(*state, kj::mv(failure));
  auto& finalFailure = KJ_ASSERT_NONNULL(result.tryGet<kj::Exception>());
  KJ_EXPECT(finalFailure.getDescription().contains("first attempt exceeded deadline"));
  KJ_EXPECT(finalFailure.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none);
  KJ_EXPECT(observer->retryCallTypes.size() == 0);
  KJ_EXPECT(observer->outcomes.size() == 0);
}

KJ_TEST("actor calls do not retry when retry requests are disabled") {
  TestTimerChannel timer;
  auto observer = kj::refcounted<RecordingObserver>();
  auto state = kj::rc<ActorCallRetryState>(timer, *observer,
      ActorCallRetryState::Config{
        .callType = ActorRetryCallType::JSRPC,
        .observationEnabled = ActorRetryGateEnabled::YES,
        .enforcementEnabled = ActorRetryGateEnabled::NO,
        .payloadReplayable = ActorCallPayloadReplayable::YES,
      });

  auto first = startAttempt(*state);
  KJ_EXPECT(KJ_ASSERT_NONNULL(first.getMetadata()).retryGateEnabled == ActorRetryGateEnabled::NO);
  KJ_EXPECT(!state->isRetryEnabled());
  KJ_EXPECT(observer->outcomes.size() == 0);
}

}  // namespace
}  // namespace workerd::api
