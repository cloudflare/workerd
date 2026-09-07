// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-call-retry.h"

#include <workerd/jsg/util.h>
#include <workerd/util/entropy.h>

#include <random>

namespace workerd::api {

ActorCallRetryState::ActorCallRetryState(
    TimerChannel& timer, RequestObserver& observer, Config config)
    : timer(timer),
      observer(kj::addRef(observer)),
      config(config) {
  retriesEnabled = config.observationEnabled.toBool() && config.enforcementEnabled.toBool();
  if (config.payloadReplayable.toBool()) {
    metadata = generateActorRetryRequestMetadata(
        kj::systemCoarseCalendarClock().now(), config.enforcementEnabled);
    if (retriesEnabled) {
      deadline = timer.nowForLimitTimeout() + RETRY_BUDGET;
    }
  }
}

ActorCallRetryState::~ActorCallRetryState() noexcept(false) {
  if (retryStartTime != kj::none && !recordedOutcome) {
    recordOutcome(ActorRetryOutcome::OTHER);
  }
}

kj::OneOf<ActorCallRetryState::Attempt, kj::Exception> ActorCallRetryState::startAttempt() {
  auto isFirstAttempt = IsFirstActorCallAttempt(attemptCount == 1);
  if (!isFirstAttempt.toBool()) {
    KJ_IF_SOME(deadlineValue, deadline) {
      if (timer.nowForLimitTimeout() >= deadlineValue) {
        recordOutcome(ActorRetryOutcome::RETRIES_EXHAUSTED);
        return KJ_ASSERT_NONNULL(originalDisconnect).clone();
      }
    }
    ++retryAttemptsStarted;
    observer->recordActorRetry(config.callType);
  }
  return Attempt(metadata, isFirstAttempt);
}

kj::OneOf<kj::Duration, kj::Exception> ActorCallRetryState::handleAttemptFailure(
    kj::Exception exception) {
  if (exception.getType() == kj::Exception::Type::DISCONNECTED) {
    startRetryLatencyTimer();
  }
  KJ_IF_SOME(claimRejection, handleClaimRejection(exception)) {
    return kj::mv(claimRejection);
  }

  auto decision = checkCanRetry(kj::mv(exception));
  KJ_SWITCH_ONEOF(decision) {
    KJ_CASE_ONEOF(plan, RetryPlan) {
      return prepareRetry(kj::mv(plan));
    }
    KJ_CASE_ONEOF(finalFailure, kj::Exception) {
      return kj::mv(finalFailure);
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<kj::Exception> ActorCallRetryState::handleClaimRejection(const kj::Exception& exception) {
  if (exception.getDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID) == kj::none) {
    return kj::none;
  }

  recordOutcome(ActorRetryOutcome::CLAIM_REJECTED);
  return KJ_ASSERT_NONNULL(originalDisconnect).clone();
}

kj::OneOf<ActorCallRetryState::RetryPlan, kj::Exception> ActorCallRetryState::checkCanRetry(
    kj::Exception exception) {
  if (exception.getType() != kj::Exception::Type::DISCONNECTED) {
    recordOutcome(ActorRetryOutcome::UNABLE_TO_RETRY);
    return kj::mv(exception);
  }
  if (exception.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none) {
    recordOutcome(ActorRetryOutcome::UNABLE_TO_RETRY);
    return kj::mv(exception);
  }
  if (!config.payloadReplayable.toBool()) {
    recordOutcome(ActorRetryOutcome::UNABLE_TO_RETRY);
    return kj::mv(exception);
  }
  if (!retriesEnabled) {
    recordOutcome(ActorRetryOutcome::UNABLE_TO_RETRY);
    return kj::mv(exception);
  }
  if (attemptCount >= MAX_ATTEMPTS) {
    recordOutcome(ActorRetryOutcome::RETRIES_EXHAUSTED);
    return KJ_ASSERT_NONNULL(originalDisconnect).clone();
  }

  auto delay = retryDelay();
  auto deadline = KJ_ASSERT_NONNULL(this->deadline);
  if (timer.nowForLimitTimeout() + delay >= deadline) {
    recordOutcome(ActorRetryOutcome::RETRIES_EXHAUSTED);
    KJ_IF_SOME(original, originalDisconnect) {
      return original.clone();
    }
    return kj::mv(exception);
  }
  return RetryPlan{kj::mv(exception), delay};
}

kj::Duration ActorCallRetryState::prepareRetry(RetryPlan plan) {
  if (attemptCount == 1) {
    originalDisconnect = plan.failure.clone();
  }
  if (plan.failure.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none) {
    KJ_ASSERT_NONNULL(metadata).isRetry = IsActorRetry::YES;
  } else if (KJ_ASSERT_NONNULL(metadata).isRetry == IsActorRetry::NO) {
    metadata = generateActorRetryRequestMetadata(
        kj::systemCoarseCalendarClock().now(), config.enforcementEnabled);
  }

  ++attemptCount;
  return plan.delay;
}

void ActorCallRetryState::recordRecovered() {
  if (originalDisconnect != kj::none) {
    recordOutcome(ActorRetryOutcome::RECOVERED);
  }
}

void ActorCallRetryState::recordCanceled() {
  recordOutcome(ActorRetryOutcome::CANCELED);
}

kj::Duration ActorCallRetryState::retryDelay() {
  static thread_local auto generator = [] {
    uint64_t seed;
    getEntropy(kj::asBytes(seed));
    return std::mt19937_64(seed);
  }();
  auto maximum = INITIAL_BACKOFF * (1u << (attemptCount - 1));
  std::uniform_int_distribution<uint64_t> distribution(0, maximum / kj::NANOSECONDS);
  return distribution(generator) * kj::NANOSECONDS;
}

void ActorCallRetryState::startRetryLatencyTimer() {
  if (retryStartTime == kj::none) {
    retryStartTime = kj::systemPreciseMonotonicClock().now();
  }
}

void ActorCallRetryState::recordOutcome(ActorRetryOutcome outcome) {
  if (recordedOutcome) return;
  recordedOutcome = true;
  if (retryAttemptsStarted == 0) return;
  auto startTime = KJ_ASSERT_NONNULL(retryStartTime);
  observer->recordActorRetryOutcome(
      config.callType, outcome, kj::systemPreciseMonotonicClock().now() - startTime);
}

}  // namespace workerd::api
