// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-channels.h>
#include <workerd/io/observer.h>
#include <workerd/util/strong-bool.h>

#include <kj/exception.h>
#include <kj/one-of.h>
#include <kj/refcount.h>
#include <kj/time.h>

namespace workerd::api {

WD_STRONG_BOOL(ActorCallRetriesAllowed);
WD_STRONG_BOOL(IsFirstActorCallAttempt);

// Retry behavior carried from a Durable Object namespace binding that configures one. Applied when
// DURABLE_OBJECT_RETRIES_USERLAND is enabled. A binding without a retry policy runs under
// ActorRetryPolicy::systemDefault() instead.
struct UserDefinedRetryPolicy {
  // Matches the runtime's default of five attempts in total.
  static constexpr uint DEFAULT_MAX_ATTEMPTS = 4;
  static constexpr uint MAX_CONFIGURABLE_ATTEMPTS = 10;

  // Retries after the initial attempt, so the total attempt count is one more than this.
  uint maxAttempts = DEFAULT_MAX_ATTEMPTS;
};

// The retry limits a single actor call runs under, resolved from either the runtime's defaults or
// a binding's UserDefinedRetryPolicy. Everything downstream reads this and never asks which.
class ActorRetryPolicy {
 public:
  static ActorRetryPolicy systemDefault() {
    return ActorRetryPolicy(SYSTEM_DEFAULT_MAX_ATTEMPTS);
  }
  static ActorRetryPolicy userDefined(const UserDefinedRetryPolicy& policy) {
    return ActorRetryPolicy(1 + policy.maxAttempts);
  }

  // Total attempts, including the initial one.
  uint maxAttempts() const {
    return attempts;
  }

 private:
  static constexpr uint SYSTEM_DEFAULT_MAX_ATTEMPTS = 5;

  explicit ActorRetryPolicy(uint attempts): attempts(attempts) {}

  uint attempts;
};

class ActorCallRetryState final: public kj::Refcounted {
 public:
  struct Config {
    ActorRetryCallType callType;
    ActorRetryGateEnabled observationEnabled;
    ActorRetryGateEnabled enforcementEnabled;
    ActorCallPayloadReplayable payloadReplayable;
  };

  class Attempt {
   public:
    Attempt(kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> metadata,
        IsFirstActorCallAttempt isFirstAttempt)
        : metadata(kj::mv(metadata)),
          isFirstAttempt(isFirstAttempt) {
      KJ_REQUIRE(isFirstAttempt.toBool() || this->metadata != kj::none,
          "actor call retry attempt requires retry metadata");
    }

    bool hasMetadata() const {
      return metadata != kj::none;
    }
    kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> getMetadata() const {
      return metadata;
    }
    kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> takeMetadata() {
      return kj::mv(metadata);
    }
    CountSubrequest getCountSubrequest() const {
      return CountSubrequest(isFirstAttempt.toBool());
    }
    IsFirstActorCallAttempt getIsFirstAttempt() const {
      return isFirstAttempt;
    }

   private:
    kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> metadata;
    IsFirstActorCallAttempt isFirstAttempt;
  };

  // `callStart` is when the logical call began, and the retry timeout runs from it. A redirected
  // request passes the original call's start, so one timeout covers the whole chain.
  ActorCallRetryState(TimerChannel& timer,
      RequestObserver& observer,
      Config config,
      ActorRetryPolicy policy,
      kj::TimePoint callStart);
  ~ActorCallRetryState() noexcept(false);

  kj::OneOf<Attempt, kj::Exception> startAttempt();
  kj::OneOf<kj::Duration, kj::Exception> handleAttemptFailure(kj::Exception exception);
  kj::Exception handleCommittedAttemptFailure(kj::Exception exception);

  kj::Exception getOriginalDisconnect() const {
    return KJ_ASSERT_NONNULL(originalDisconnect).clone();
  }

  bool isRetryEnabled() const {
    return retriesEnabled;
  }
  kj::TimePoint getCallStart() const {
    return callStart;
  }
  // Rejects `promise` with the original disconnect if the retry timeout expires first. The first
  // attempt is never cancelled, so this only affects retries.
  template <typename T>
  kj::Promise<T> enforceRetryTimeout(kj::Promise<T> promise) {
    if (attemptCount == 1) return kj::mv(promise);
    auto timeout = timer.atLimitTimeout(retryCutoff())
                       .then([self = addRefToThis()]() mutable -> kj::Promise<T> {
      return self->retryTimeoutExpired();
    });
    return kj::mv(promise).exclusiveJoin(kj::mv(timeout));
  }
  void maybeStartRetryLatencyTimer(const kj::Exception& exception);
  void recordRecovered();
  void recordCanceled();

 private:
  static constexpr auto RETRY_BUDGET = 10 * kj::SECONDS;
  static constexpr auto INITIAL_BACKOFF = 500 * kj::MILLISECONDS;
  static constexpr auto MAX_BACKOFF = 2 * kj::SECONDS;

  IoChannelFactory::ActorRetryRequestMetadata freshMetadata() const;
  // No retry may start, or still be running, at or after this point.
  kj::TimePoint retryCutoff() const {
    return callStart + RETRY_BUDGET;
  }
  kj::Exception retryTimeoutExpired();
  kj::Maybe<kj::Exception> handleClaimRejection(const kj::Exception& exception);
  kj::OneOf<kj::Duration, kj::Exception> checkCanRetry(kj::Exception exception);
  kj::Duration retryDelay();
  void recordOutcome(ActorRetryOutcome outcome);

  TimerChannel& timer;
  kj::Own<RequestObserver> observer;
  Config config;
  ActorRetryPolicy policy;
  kj::TimePoint callStart;
  bool retriesEnabled;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> metadata;
  kj::Maybe<kj::Exception> originalDisconnect;
  kj::Maybe<kj::TimePoint> retryStartTime;
  uint attemptCount = 1;
  uint retryAttemptsStarted = 0;
  bool recordedOutcome = false;
};

}  // namespace workerd::api
