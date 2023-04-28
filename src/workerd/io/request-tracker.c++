#include "request-tracker.h"

namespace workerd {

RequestTracker::RequestTracker(Hooks& hooksImpl) : hooks(hooksImpl) {};

RequestTracker::~RequestTracker() noexcept(false) {}

RequestTracker::ActiveRequest::ActiveRequest(kj::Badge<RequestTracker>, RequestTracker& parent)
  : maybeParent(kj::addRef(parent)) {
  parent.requestActive();
}
RequestTracker::ActiveRequest::~ActiveRequest() noexcept(false) {
  KJ_IF_MAYBE(p, maybeParent) {
    p->get()->requestInactive();
  }
}

void RequestTracker::requestActive() {
  if (activeRequests++ == 0) {
    KJ_IF_MAYBE(h, hooks) {
      h->active();
    }
  }
}

void RequestTracker::requestInactive() {
  KJ_IF_MAYBE(h, hooks) {
    if (--activeRequests == 0) {
      h->inactive();
    }
  }
}

RequestTracker::ActiveRequest RequestTracker::startRequest() {
  return ActiveRequest({}, *this);
}

} // namespace workerd
