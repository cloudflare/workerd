// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-call-retry.h"

#include <workerd/jsg/util.h>
#include <workerd/util/entropy.h>

#include <random>

namespace workerd::api {

ActorCallRetryState::ActorCallRetryState(TimerChannel& timer,
    RequestObserver& observer,
    Config config,
    ActorRetryPolicy policy,
    kj::TimePoint callStart)
    : timer(timer),
      observer(kj::addRef(observer)),
      config(config),
      policy(policy),
      callStart(callStart),
      retriesEnabled(config.payloadReplayable.toBool() && config.observationEnabled.toBool() &&
          config.enforcementEnabled.toBool() && policy.maxAttempts() > 1),
      // A call that cannot retry has no retry policy to enforce, so it runs under the default
      // window regardless of what the policy says.
      timeLimit(retriesEnabled ? policy.makeTimeLimit(timer, callStart)
                               : ActorCallTimeLimit::retryWindow(timer)) {
  if (config.payloadReplayable.toBool()) {
    metadata = freshMetadata();
  }
}

ActorCallRetryState::~ActorCallRetryState() noexcept(false) {
  if (retryStartTime != kj::none && !recordedOutcome) {
    recordOutcome(ActorRetryOutcome::OTHER);
  }
}

kj::Exception ActorCallTimeLimit::deadlineExceeded() {
  return JSG_KJ_EXCEPTION(OVERLOADED, Error, "Durable Object request exceeded max_duration_ms");
}

IoChannelFactory::ActorRetryRequestMetadata ActorCallRetryState::freshMetadata() const {
  return generateActorRetryRequestMetadata(
      kj::systemCoarseCalendarClock().now(), config.enforcementEnabled);
}

kj::OneOf<ActorCallRetryState::Attempt, kj::Exception> ActorCallRetryState::startAttempt() {
  auto isFirstAttempt = IsFirstActorCallAttempt(attemptCount == 1);
  // A RETRY_WINDOW only bounds retries; a CALL_DEADLINE bounds every attempt.
  if ((!isFirstAttempt.toBool() || timeLimit.isDeadline()) &&
      timer.nowForLimitTimeout() >= timeLimit.cutoff) {
    return timeLimitExceeded();
  }
  if (!isFirstAttempt.toBool()) {
    ++retryAttemptsStarted;
    observer->recordActorRetry(config.callType);
  }
  return Attempt(metadata, isFirstAttempt);
}

kj::Exception ActorCallRetryState::timeLimitExceeded() {
  recordOutcome(ActorRetryOutcome::RETRY_BUDGET_EXHAUSTED);
  KJ_IF_SOME(disconnect, originalDisconnect) {
    return disconnect.clone();
  }
  // Only a CALL_DEADLINE can run out before anything has disconnected.
  KJ_ASSERT(timeLimit.isDeadline());
  return ActorCallTimeLimit::deadlineExceeded();
}

kj::OneOf<kj::Duration, kj::Exception> ActorCallRetryState::handleAttemptFailure(
    kj::Exception exception) {
  maybeStartRetryLatencyTimer(exception);
  KJ_IF_SOME(claimRejection, handleClaimRejection(exception)) {
    return kj::mv(claimRejection);
  }

  return checkCanRetry(kj::mv(exception));
}

void ActorCallRetryState::maybeStartRetryLatencyTimer(const kj::Exception& exception) {
  if (exception.getType() == kj::Exception::Type::DISCONNECTED && retryStartTime == kj::none) {
    retryStartTime = kj::systemPreciseMonotonicClock().now();
  }
}

kj::Maybe<kj::Exception> ActorCallRetryState::handleClaimRejection(const kj::Exception& exception) {
  if (exception.getDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID) == kj::none) {
    return kj::none;
  }

  recordOutcome(ActorRetryOutcome::CLAIM_REJECTED);
  return KJ_ASSERT_NONNULL(
      originalDisconnect, "actor retry claim rejected before a retry was attempted")
      .clone();
}

kj::OneOf<kj::Duration, kj::Exception> ActorCallRetryState::checkCanRetry(kj::Exception exception) {
  if (exception.getType() != kj::Exception::Type::DISCONNECTED) {
    recordOutcome(ActorRetryOutcome::UNABLE_TO_RETRY);
    return kj::mv(exception);
  }
  if (exception.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none) {
    recordOutcome(ActorRetryOutcome::UNABLE_TO_RETRY);
    return kj::mv(exception);
  }
  if (originalDisconnect == kj::none) {
    originalDisconnect = exception.clone();
  }
  if (attemptCount >= policy.maxAttempts()) {
    recordOutcome(ActorRetryOutcome::ATTEMPTS_EXHAUSTED);
    return KJ_ASSERT_NONNULL(originalDisconnect).clone();
  }

  auto delay = retryDelay();
  if (timer.nowForLimitTimeout() + delay >= timeLimit.cutoff) {
    return timeLimitExceeded();
  }
  if (exception.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none) {
    KJ_ASSERT_NONNULL(metadata).isRetry = IsActorRetry::YES;
  } else if (KJ_ASSERT_NONNULL(metadata).isRetry == IsActorRetry::NO) {
    metadata = freshMetadata();
  }

  ++attemptCount;
  return delay;
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
  auto maximum = kj::min(INITIAL_BACKOFF * (1u << (attemptCount - 1)), MAX_BACKOFF);
  std::uniform_int_distribution<uint64_t> distribution(0, maximum / kj::NANOSECONDS);
  return distribution(generator) * kj::NANOSECONDS;
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
