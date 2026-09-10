// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
#include <kj/async.h>
#include <kj/common.h>
#include <kj/debug.h>
#include <kj/function.h>
#include <kj/list.h>
#include <kj/refcount.h>

namespace workerd {

// A canceler that combines a kj::Canceler with sticky cancellation state and observer
// callbacks and (unlike kj::Canceler, whose destructor implicitly cancels) releases
// any still-wrapped promises when dropped without having been canceled.
//
// This is used, for instance, to support fetch() requests that use an AbortSignal:
// the signal's abort registration cancels it (via a reference whose validity the
// registration's RAII handle guarantees; see api::AbortSignal), while wrappers like
// AbortableInputStream wrap their promises through it and observe cancellation through
// Listener.
class ReleasingCanceler final {
 public:
  // Invokes fn when the canceler is canceled. If the canceler was ALREADY canceled at
  // registration time, fn is invoked immediately. The fn is invoked at most once.
  //
  // A listener is only linked to the canceler while it is still awaiting cancellation: once
  // it has fired (or if it registered after cancellation), it no longer references the
  // canceler and may safely outlive it. A listener that has NOT yet fired must be destroyed
  // before the canceler.
  class Listener final {
   public:
    explicit Listener(ReleasingCanceler& canceler, kj::Function<void()> fn);
    ~Listener() noexcept(false);

    // Also implied by the ListLink member (and the reference member, for assignment), but
    // stated explicitly for clarity and better diagnostics: a linked Listener's address must
    // remain stable, and it must not outlive its canceler.
    KJ_DISALLOW_COPY_AND_MOVE(Listener);

   private:
    kj::Function<void()> fn;
    ReleasingCanceler& canceler;
    kj::ListLink<Listener> link;

    friend class ReleasingCanceler;
  };

  ReleasingCanceler(kj::Maybe<kj::Exception> reason = kj::none);

  ~ReleasingCanceler() noexcept(false);

  KJ_DISALLOW_COPY_AND_MOVE(ReleasingCanceler);

  template <typename T>
  kj::Promise<T> wrap(kj::Promise<T> promise) {
    KJ_IF_SOME(ex, reason) {
      return ex.clone();
    }
    return canceler.wrap(kj::mv(promise));
  }

  void cancel(const kj::Exception& exception);

  void throwIfCanceled();

  bool isCanceled() const;

 private:
  kj::Canceler canceler;
  kj::Maybe<kj::Exception> reason;

  void addListener(Listener& listener);
  void removeListener(Listener& listener);

  kj::List<Listener, &Listener::link> listeners;
};

}  // namespace workerd
