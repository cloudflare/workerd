#include "request-tracker.h"

namespace workerd {

RequestTracker::Impl::Impl(Hooks& hooksImpl) : hooks(hooksImpl) {};
RequestTracker::RequestTracker(Hooks& hooksImpl) : impl(kj::refcounted<Impl>(hooksImpl)) {};

RequestTracker::~RequestTracker() noexcept(false) {
  // Since we're destroying the RequestTracker, we want to prevent any hooks from running after
  // this point.
  impl->hooks = nullptr;
}

RequestTracker::ActiveRequest::ActiveRequest(RequestTracker::Impl& parentRef)
    : parent(kj::addRef(parentRef)) {
  if (parentRef.activeRequests++ == 0) {
    KJ_IF_MAYBE(h, parentRef.hooks) {
      h->active();
    }
  }
}

RequestTracker::ActiveRequest::~ActiveRequest() {
  KJ_IF_MAYBE(p, parent) {
    KJ_IF_MAYBE(h, (*p)->hooks) {
      if (--(*p)->activeRequests == 0) {
        h->inactive();
      }
    }
  }
}

RequestTracker::ActiveRequest RequestTracker::startRequest() {
  return ActiveRequest(*this->impl.get());
}

} // namespace workerd
