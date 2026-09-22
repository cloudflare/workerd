// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "canceler.h"

namespace workerd {

ReleasingCanceler::Listener::Listener(ReleasingCanceler& canceler, kj::Function<void()> fn)
    : fn(kj::mv(fn)),
      canceler(canceler) {
  canceler.addListener(*this);
}

ReleasingCanceler::Listener::~Listener() noexcept(false) {
  if (link.isLinked()) {
    canceler.removeListener(*this);
  }
}

ReleasingCanceler::ReleasingCanceler(kj::Maybe<kj::Exception> reason): reason(kj::mv(reason)) {}

ReleasingCanceler::~ReleasingCanceler() noexcept(false) {
  // `listeners` only contains listeners still awaiting cancellation, and those must not
  // outlive the canceler. (Listeners that already fired were unlinked and may live on.)
  KJ_ASSERT(listeners.empty());

  // Release rather than cancel any remaining wrapped promises: dropping the canceler
  // without an explicit cancellation must not reject the wrapped work.
  canceler.release();
}

void ReleasingCanceler::cancel(const kj::Exception& exception) {
  if (reason == kj::none) {
    reason = exception.clone();
    canceler.cancel(exception);

    // Drain the list, unlinking each listener before invoking it: a fired listener can never
    // fire again, so there is no reason to retain it, and once unlinked its lifetime is
    // decoupled from this canceler (its destructor no longer needs to touch us). Unlinking
    // first also makes it safe for a callback to destroy its own — or any other — listener.
    //
    // Re-entrancy (mirroring kj::Canceler::cancel()'s unlink-then-invoke drain): a callback
    // may call cancel() again (no-op, `reason` is already set), register new listeners (they
    // fire immediately without linking, see addListener()), or destroy pending listeners
    // (their destructors self-remove from the live list, which is re-consulted on every
    // iteration). It is the callback's responsibility NOT to destroy the canceler itself,
    // and not to throw.
    while (!listeners.empty()) {
      auto& listener = listeners.front();
      listeners.remove(listener);
      listener.fn();
    }
  }
}

void ReleasingCanceler::throwIfCanceled() {
  KJ_IF_SOME(ex, reason) {
    kj::throwFatalException(ex.clone());
  }
}

bool ReleasingCanceler::isCanceled() const {
  return reason != kj::none;
}

void ReleasingCanceler::addListener(Listener& listener) {
  if (reason != kj::none) {
    // The canceler was already canceled; a listener registered now would otherwise never be
    // notified. Fire it immediately — without ever linking it, since it cannot fire again —
    // so that late registrants observe cancellation the same way wrap() does (which returns
    // the exception immediately).
    listener.fn();
    return;
  }
  listeners.add(listener);
}

void ReleasingCanceler::removeListener(Listener& listener) {
  listeners.remove(listener);
}

}  // namespace workerd
