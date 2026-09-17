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
WD_STRONG_BOOL(ActorCallRetriesAllowed);
WD_STRONG_BOOL(IsFirstActorCallAttempt);

// Retry behavior carried from a Durable Object namespace binding that configures one. Applied when
// DURABLE_OBJECT_RETRIES_USERLAND is enabled. A binding without a retry policy runs under
// ActorRetryPolicy::systemDefault() instead.
struct UserDefinedRetryPolicy {
  // Matches the runtime's default of five attempts in total.
  static constexpr uint DEFAULT_MAX_RETRY_ATTEMPTS = 4;
  static constexpr uint MAX_CONFIGURABLE_RETRY_ATTEMPTS = 10;
  static constexpr auto MAX_CONFIGURABLE_DURATION = 60 * kj::SECONDS;

  // Retries after the initial attempt, so the total attempt count is one more than this.
  uint maxRetryAttempts = DEFAULT_MAX_RETRY_ATTEMPTS;
  // Bound on the whole logical call. See ActorCallTimeLimit. When omitted, the runtime's retry
  // window applies. Like every retry setting, it has no effect on a call that cannot retry, whether
  // because maxRetryAttempts is zero or because the request body is not replayable.
  kj::Maybe<kj::Duration> maxDuration;
};

// The time limit on an actor call has one of two meanings.
//
// Both run from when the retry state is created, after any output-gate wait.
//
// RETRY_WINDOW bounds when a retry may start and never interrupts an attempt already in flight.
// This is the runtime's default.
//
// CALL_DEADLINE bounds the whole logical call: the initial attempt, backoff, retries, and
// redirects. Whatever is in flight when it passes is interrupted. This is what an explicit
// max_duration_ms configures.
struct ActorCallTimeLimit {
  enum class Kind { RETRY_WINDOW, CALL_DEADLINE };

  static constexpr auto RETRY_WINDOW_LENGTH = 10 * kj::SECONDS;

  static ActorCallTimeLimit retryWindow(TimerChannel& timer) {
    return {Kind::RETRY_WINDOW, timer.nowForLimitTimeout() + RETRY_WINDOW_LENGTH};
  }
  static ActorCallTimeLimit callDeadline(kj::TimePoint deadline) {
    return {Kind::CALL_DEADLINE, deadline};
  }

  // The error the caller sees when a CALL_DEADLINE passes before any attempt has disconnected.
  static kj::Exception deadlineExceeded();

  bool isDeadline() const {
    return kind == Kind::CALL_DEADLINE;
  }

  Kind kind;
  // No retry may start at or after this point. Under CALL_DEADLINE, nothing may be in flight
  // either.
  kj::TimePoint cutoff;
};

// The retry limits a single actor call runs under, resolved from either the runtime's defaults or
// a binding's UserDefinedRetryPolicy. Everything downstream reads this and never asks which.
class ActorRetryPolicy {
 public:
  static ActorRetryPolicy systemDefault() {
    return ActorRetryPolicy(SYSTEM_DEFAULT_MAX_ATTEMPTS, kj::none);
  }
  static ActorRetryPolicy userDefined(const UserDefinedRetryPolicy& policy) {
    return ActorRetryPolicy(1 + policy.maxRetryAttempts, policy.maxDuration);
  }

  // Total attempts, including the initial one.
  uint maxAttempts() const {
    return attempts;
  }
  // A CALL_DEADLINE from `callStart` if the policy bounds duration and allows retries, otherwise
  // the default RETRY_WINDOW.
  ActorCallTimeLimit makeTimeLimit(TimerChannel& timer, kj::TimePoint callStart) const {
    if (attempts > 1) {
      KJ_IF_SOME(d, duration) {
        return ActorCallTimeLimit::callDeadline(callStart + d);
      }
    }
    return ActorCallTimeLimit::retryWindow(timer);
  }

 private:
  static constexpr uint SYSTEM_DEFAULT_MAX_ATTEMPTS = 5;

  ActorRetryPolicy(uint attempts, kj::Maybe<kj::Duration> duration)
      : attempts(attempts),
        duration(duration) {}

  uint attempts;
  kj::Maybe<kj::Duration> duration;
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

  // `callStart` is when the logical call began. A redirected request passes the original call's
  // start so a CALL_DEADLINE spans the whole chain.
  ActorCallRetryState(TimerChannel& timer,
      RequestObserver& observer,
      Config config,
      ActorRetryPolicy policy,
      kj::TimePoint callStart);
  ~ActorCallRetryState() noexcept(false);

  kj::OneOf<Attempt, kj::Exception> startAttempt();
  kj::OneOf<kj::Duration, kj::Exception> handleAttemptFailure(kj::Exception exception);

  bool isRetryEnabled() const {
    return retriesEnabled;
  }
  kj::TimePoint getCallStart() const {
    return callStart;
  }
  // Rejects `promise` when a CALL_DEADLINE passes. No-op under a RETRY_WINDOW.
  template <typename T>
  kj::Promise<T> enforceDeadline(kj::Promise<T> promise) {
    if (!timeLimit.isDeadline()) return kj::mv(promise);
    if (timer.nowForLimitTimeout() >= timeLimit.cutoff) {
      return timeLimitExceeded();
    }
    auto timeout = timer.atLimitTimeout(timeLimit.cutoff)
                       .then([self = addRefToThis()]() mutable -> kj::Promise<T> {
      return self->timeLimitExceeded();
    });
    return kj::mv(promise).exclusiveJoin(kj::mv(timeout));
  }
  void maybeStartRetryLatencyTimer(const kj::Exception& exception);
  void recordRecovered();
  void recordCanceled();

 private:
  static constexpr auto INITIAL_BACKOFF = 500 * kj::MILLISECONDS;
  static constexpr auto MAX_BACKOFF = 2 * kj::SECONDS;

  IoChannelFactory::ActorRetryRequestMetadata freshMetadata() const;
  // Returns the original disconnect, or the deadline error when nothing has disconnected yet.
  kj::Exception timeLimitExceeded();
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
  // Initialized from `retriesEnabled`, so it must stay declared after it.
  ActorCallTimeLimit timeLimit;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> metadata;
  kj::Maybe<kj::Exception> originalDisconnect;
  kj::Maybe<kj::TimePoint> retryStartTime;
  uint attemptCount = 1;
  uint retryAttemptsStarted = 0;
  bool recordedOutcome = false;
};

}  // namespace workerd::api
