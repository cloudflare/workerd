// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async.h>
#include <kj/mutex.h>

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
    return kj::atomicRefcounted<XThreadNotifier>();
  }

  XThreadNotifier() : paf(kj::newPromiseAndCrossThreadFulfiller<void>()) { }

  kj::Promise<void> awaitNotification() {
    auto promise = kj::mv(paf.lockExclusive()->promise);
    co_await promise;
    auto lockedPaf = paf.lockExclusive();
    auto nextPaf = kj::newPromiseAndCrossThreadFulfiller<void>();
    lockedPaf->promise = kj::mv(nextPaf.promise);
    lockedPaf->fulfiller = kj::mv(nextPaf.fulfiller);
  }

  void notify() const {
    paf.lockExclusive()->fulfiller->fulfill();
  }

private:
  kj::MutexGuarded<kj::PromiseCrossThreadFulfillerPair<void>> paf;
};


// Convenience struct for creating and passing around a kj::Executor and XThreadNotifier. The
// default constructor creates a pair of the objects which are both tied to the current thread.
struct ExecutorNotifierPair {
  kj::Own<const kj::Executor> executor = kj::getCurrentThreadExecutor().addRef();
  kj::Own<XThreadNotifier> notifier = XThreadNotifier::create();

  ExecutorNotifierPair clone() {
    return {
      .executor = executor->addRef(),
      .notifier = kj::atomicAddRef(*notifier),
    };
  }
};

}  // namespace workerd
