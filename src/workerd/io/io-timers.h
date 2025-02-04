#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd {

class IoContext;

// A TimeoutId is a positive non-zero integer value that explicitly identifies a timeout set on an
// isolate.
//
// Lastly, timeout ids can experience integer roll over. It is expected that the
// setTimeout/clearTimeout implementation will enforce id uniqueness for *active* timeouts. This
// does not mean that an external user cannot have cached a timeout id for a long expired timeout.
// However, clearTimeout implementations are expected to only have access to timeouts set via that
// same implementation.
class TimeoutId {
 public:
  // Use a double so that we can exceed the maximum value for uint32_t.
  using NumberType = double;

  // Store as a uint64_t so that we treat this id as an integer.
  using ValueType = uint64_t;

  class Generator;

  // Convert an externally provided double into a TimeoutId. If you are making a new TimeoutId,
  // use a Generator instead.
  inline static TimeoutId fromNumber(NumberType id) {
    return TimeoutId(ValueType(id));
  }

  // Convert a TimeoutId to an integer-convertable double for external consumption.
  // Note that this is expected to be less than or equal to JavaScript Number.MAX_SAFE_INTEGER
  // (2^53 - 1). To reach greater than that value in normal operation, we'd need a Generator to
  // live far far longer than our normal release/restart cycle, be initialized with a large
  // starting value, or experience active concurrency _somehow_.
  inline NumberType toNumber() const {
    return value;
  }

  inline bool operator<(TimeoutId id) const {
    return value < id.value;
  }

 private:
  constexpr explicit TimeoutId(ValueType value): value(value) {}

  ValueType value;
};

class TimeoutId::Generator {
 public:
  Generator() = default;
  KJ_DISALLOW_COPY_AND_MOVE(Generator);

  // Get the next TimeoutId for this generator. This function will never return a TimeoutId <= 0.
  TimeoutId getNext();

 private:
  // We always skip 0 per the spec:
  // https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#timers.
  TimeoutId::ValueType nextId = 1;
};

class TimeoutManager {
 public:
  // Upper bound on the number of timeouts a user can *ever* have active.
  constexpr static auto MAX_TIMEOUTS = 10'000;

  struct TimeoutParameters {
    TimeoutParameters(bool repeat, int64_t msDelay, jsg::Function<void()> function);

    bool repeat;
    int64_t msDelay;

    // This is a maybe to allow cancel to clear it and free the reference
    // when it is no longer needed.
    kj::Maybe<jsg::Function<void()>> function;
  };

  virtual TimeoutId setTimeout(
      IoContext& context, TimeoutId::Generator& generator, TimeoutParameters params) = 0;
  virtual void clearTimeout(IoContext& context, TimeoutId id) = 0;
  virtual size_t getTimeoutCount() const = 0;
  virtual kj::Maybe<kj::Date> getNextTimeout() const = 0;
};

}  // namespace workerd
