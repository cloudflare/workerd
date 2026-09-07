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

WD_STRONG_BOOL(ActorCallPayloadReplayable);
WD_STRONG_BOOL(IsFirstActorCallAttempt);

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

  ActorCallRetryState(TimerChannel& timer, RequestObserver& observer, Config config);
  ~ActorCallRetryState() noexcept(false);

  kj::OneOf<Attempt, kj::Exception> startAttempt();
  kj::OneOf<kj::Duration, kj::Exception> handleAttemptFailure(kj::Exception exception);

  void recordRecovered();
  void recordCanceled();

 private:
  struct RetryPlan {
    kj::Exception failure;
    kj::Duration delay;
  };

  static constexpr uint MAX_ATTEMPTS = 5;
  static constexpr auto RETRY_BUDGET = 10 * kj::SECONDS;
  static constexpr auto INITIAL_BACKOFF = 50 * kj::MILLISECONDS;

  kj::Maybe<kj::Exception> handleClaimRejection(const kj::Exception& exception);
  kj::OneOf<RetryPlan, kj::Exception> checkCanRetry(kj::Exception exception);
  kj::Duration prepareRetry(RetryPlan plan);
  kj::Duration retryDelay();
  void startRetryLatencyTimer();
  void recordOutcome(ActorRetryOutcome outcome);

  TimerChannel& timer;
  kj::Own<RequestObserver> observer;
  Config config;
  bool retriesEnabled;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> metadata;
  kj::Maybe<kj::TimePoint> deadline;
  kj::Maybe<kj::Exception> originalDisconnect;
  kj::Maybe<kj::TimePoint> retryStartTime;
  uint attemptCount = 1;
  uint retryAttemptsStarted = 0;
  bool recordedOutcome = false;
};

}  // namespace workerd::api
