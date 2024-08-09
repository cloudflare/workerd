// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
#include <kj/async.h>
#include <kj/common.h>
#include <kj/debug.h>
#include <kj/function.h>
#include <kj/refcount.h>
#include <kj/list.h>

namespace workerd {

// A simple wrapper around kj::Canceler that can be safely
// shared by multiple objects. This is used, for instance,
// to support fetch() requests that use an AbortSignal.
// The AbortSignal (see api/basics.h) creates an instance
// of RefcountedCanceler then passes references to it out
// to various other objects that will use it to wrap their
// Promises.
class RefcountedCanceler: public kj::Refcounted {
public:
  class Listener {
  public:
    explicit Listener(RefcountedCanceler& canceler, kj::Function<void()> fn)
        : fn(kj::mv(fn)),
          canceler(canceler) {
      canceler.addListener(*this);
    }

    ~Listener() {
      canceler.removeListener(*this);
    }

  private:
    kj::Function<void()> fn;
    RefcountedCanceler& canceler;
    kj::ListLink<Listener> link;

    friend class RefcountedCanceler;
  };

  RefcountedCanceler(kj::Maybe<kj::Exception> reason = kj::none): reason(kj::mv(reason)) {}

  ~RefcountedCanceler() noexcept(false) {
    // `listeners` has to be empty since each listener should have held a strong reference.
    KJ_ASSERT(listeners.empty());

    // RefcountedCanceler is used in use cases where we don't want to cancel by default if the
    // canceler is destroyed, so release any remaining wrapped promises.
    canceler.release();
  }

  KJ_DISALLOW_COPY_AND_MOVE(RefcountedCanceler);

  template <typename T>
  kj::Promise<T> wrap(kj::Promise<T> promise) {
    KJ_IF_SOME(ex, reason) {
      return kj::cp(ex);
    }
    return canceler.wrap(kj::mv(promise));
  }

  void cancel(kj::StringPtr cancelReason) {
    if (reason == kj::none) {
      cancel(kj::Exception(
          kj::Exception::Type::DISCONNECTED, __FILE__, __LINE__, kj::str(cancelReason)));
    }
  }

  void cancel(const kj::Exception& exception) {
    if (reason == kj::none) {
      reason = kj::cp(exception);
      canceler.cancel(exception);
      for (auto& listener: listeners) {
        listener.fn();
      }
    }
  }

  bool isEmpty() const {
    return canceler.isEmpty();
  }

  void throwIfCanceled() {
    KJ_IF_SOME(ex, reason) {
      kj::throwFatalException(kj::cp(ex));
    }
  }

  bool isCanceled() const {
    return reason != kj::none;
  }

  void addListener(Listener& listener) {
    listeners.add(listener);
  }

  void removeListener(Listener& listener) {
    listeners.remove(listener);
  }

private:
  kj::Canceler canceler;
  kj::Maybe<kj::Exception> reason;

  kj::List<Listener, &Listener::link> listeners;
};

}  // namespace workerd
