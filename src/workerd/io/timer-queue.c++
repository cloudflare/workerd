// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "timer-queue.h"

#include "io-channels.h"

#include <kj/debug.h>

namespace workerd {

void TimerQueue::adoptFrom(TimerQueue& source, TimerChannel& timerChannel) {
  // Merge pending deadlines. Callbacks that have already fired are waiting on InputGate and
  // retain its waiter order, even ahead of earlier deadlines that have not yet been armed.
  source.timerTask = nullptr;
  source.trackedHead = kj::none;
  while (!source.empty()) {
    auto entry = source.queue.release(*source.queue.begin());
    queue.insert(entry.key, kj::mv(entry.value));
  }
  armNextTimeout(timerChannel);
}

void TimerQueue::armNextTimeout(TimerChannel& timerChannel) {
  kj::Maybe<Key> next;
  if (queue.size() > 0) next = queue.begin()->key;
  if (next == trackedHead) return;

  // A fired callback still owns the front of the queue until completion or cancellation.
  // Its continuation runs independently of timerTask, including while its scope is suspended.
  if (queue.size() == 0 || queue.begin()->value == kj::none) {
    timerTask = nullptr;
    trackedHead = next;
    return;
  }

  auto key = KJ_ASSERT_NONNULL(next);
  // A synchronous arming failure must allow a later queue change to retry this deadline.
  auto promise = timerChannel.atTime(key.when);
  timerTask = promise
                  // timerTask belongs to this TimerQueue and is canceled before queue is destroyed.
                  // NOLINTNEXTLINE(workerd-unsafe-continuation-capture)
                  .then([this, key]() {
    KJ_ASSERT(queue.size() > 0 && queue.begin()->key == key,
        "earliest timeout changed without calling armNextTimeout()");
    auto fulfiller = kj::mv(KJ_ASSERT_NONNULL(queue.begin()->value));
    queue.begin()->value = kj::none;
    fulfiller->fulfill();
  }).eagerlyEvaluate([](kj::Exception&& e) { KJ_LOG(ERROR, e); });
  trackedHead = next;
}

}  // namespace workerd
