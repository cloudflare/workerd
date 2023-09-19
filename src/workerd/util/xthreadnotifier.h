// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/refcount.h>
#include <kj/async.h>
#include <kj/debug.h>

namespace workerd {

class XThreadNotifier final: public kj::AtomicRefcounted {
  // Class encapsulating the ability to notify a waiting thread from other threads.
  //
  // TODO(cleanup): Can this be consolidated with wait-list.h?
  //
  // TODO(cleanup): This could be a lot simpler if only it were possible to cancel
  //   an executor.executeAsync() promise from an arbitrary thread. Then, if the inspector
  //   session was destroyed in its thread while a cross-thread notification was in-flight, it
  //   could cancel that notification directly.
public:
  static inline kj::Own<XThreadNotifier> create() {
    return kj::atomicRefcounted<XThreadNotifier>(kj::getCurrentThreadExecutor());
  }

  XThreadNotifier(const kj::Executor& executor) : executor(executor) { }

  void clear() {
    // Must call in main thread before it drops its reference.
    paf = kj::none;
  }

  kj::Promise<void> awaitNotification() {
    auto promise = kj::mv(KJ_ASSERT_NONNULL(paf).promise);
    co_await promise;
    paf = kj::newPromiseAndFulfiller<void>();
  }

  void notify() const {
    executor.executeAsync([ref = kj::atomicAddRef(*this)]() {
      KJ_IF_SOME(p, ref->paf) {
        p.fulfiller->fulfill();
      }
    }).detach([](kj::Exception&& exception) {
      KJ_LOG(ERROR, exception);
    });
  }

private:
  const kj::Executor& executor;
  mutable kj::Maybe<kj::PromiseFulfillerPair<void>> paf = kj::newPromiseAndFulfiller<void>();
  // Accessed only in notifier's owning thread.
};

}  // namespace workerd
