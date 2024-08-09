// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "request-tracker.h"

namespace workerd {

RequestTracker::RequestTracker(Hooks& hooksImpl): hooks(hooksImpl) {};

RequestTracker::~RequestTracker() noexcept(false) {}

RequestTracker::ActiveRequest::ActiveRequest(kj::Badge<RequestTracker>, RequestTracker& parent)
    : maybeParent(kj::addRef(parent)) {
  parent.requestActive();
}
RequestTracker::ActiveRequest::~ActiveRequest() noexcept(false) {
  KJ_IF_SOME(p, maybeParent) {
    p.get()->requestInactive();
  }
}

void RequestTracker::requestActive() {
  if (activeRequests++ == 0) {
    KJ_IF_SOME(h, hooks) {
      h.active();
    }
  }
}

void RequestTracker::requestInactive() {
  KJ_IF_SOME(h, hooks) {
    if (--activeRequests == 0) {
      h.inactive();
    }
  }
}

RequestTracker::ActiveRequest RequestTracker::startRequest() {
  return ActiveRequest({}, *this);
}

}  // namespace workerd
