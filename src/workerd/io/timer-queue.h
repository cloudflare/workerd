// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async.h>
#include <kj/map.h>
#include <kj/time.h>

namespace workerd {

class TimerChannel;

// Orders timer callbacks within one input-gate scope. A fulfilled entry stays in the queue until
// its owner erases it after callback completion or cancellation, so later deadlines cannot fire
// before an overdue callback has had a chance to register another timer.
//
// The timer task captures this queue's address, so queues cannot be moved.
class TimerQueue {
 public:
  TimerQueue() = default;
  KJ_DISALLOW_COPY_AND_MOVE(TimerQueue);

  struct Key {
    kj::Date when;
    kj::uint tiebreaker;  // Unique across queues that may be merged, including equal deadlines.

    inline bool operator<(const Key& other) const {
      if (when < other.when) return true;
      if (when > other.when) return false;
      return tiebreaker < other.tiebreaker;
    }
    inline bool operator==(const Key& other) const {
      return when == other.when && tiebreaker == other.tiebreaker;
    }
  };

  void clear() {
    timerTask = nullptr;
    trackedHead = kj::none;
    queue.clear();
  }

  bool empty() const {
    return queue.size() == 0;
  }

  kj::Maybe<kj::Date> getClockClamp() const {
    if (empty()) return kj::none;
    return queue.begin()->key.when;
  }

  void insert(Key key, kj::Own<kj::PromiseFulfiller<void>> fulfiller, TimerChannel& timerChannel) {
    queue.insert(key, kj::mv(fulfiller));
    armNextTimeout(timerChannel);
  }

  void erase(Key key, TimerChannel& timerChannel) {
    queue.erase(key);
    armNextTimeout(timerChannel);
  }

  void adoptFrom(TimerQueue& source, TimerChannel& timerChannel);

 private:
  // Fired timers have no fulfiller. Keep their entries until the callback completes so
  // newly registered deadlines are considered before arming another timer. Adopted callbacks
  // that have already fired retain their input-gate waiter order.
  kj::TreeMap<Key, kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>>> queue;

  // Each gate serializes its own timer callbacks. A callback waiting on a parent gate must
  // not prevent a nested critical section's timer task from firing.
  kj::Promise<void> timerTask = nullptr;
  // Records the head we armed or found already fired. Synchronous arming failures leave this
  // unchanged so later queue changes retry.
  kj::Maybe<Key> trackedHead;

  void armNextTimeout(TimerChannel& timerChannel);
};

}  // namespace workerd
