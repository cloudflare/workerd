#pragma once

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/refcount.h>

namespace workerd {

class RequestTracker {
  // This class is used to track a number of associated requests so that some desired behavior
  // is carried out once all requests have completed. `activeRequests` is incremented each time a
  // new request is created, and then decremented once it completes.

  class Impl;
public:
  class Hooks {
  public:
    virtual void active() = 0;
    virtual void inactive() = 0;
  };

  class ActiveRequest {
    // An object that should be associated with (attached to) a request.
    // On creation, if the parent RequestTracker has 0 active requests, we call the `active()` hook.
    // On destruction, if the RequestTracker has 0 active requests, we call the `inactive()` hook.
    // Otherwise, we just increment/decrement the count on creation/destruction respectively.
    ActiveRequest(RequestTracker::Impl& parentRef);
    KJ_DISALLOW_COPY(ActiveRequest);

  public:
    ActiveRequest(ActiveRequest&& other) = default;
    ~ActiveRequest();

  private:
    kj::Maybe<kj::Own<Impl>> parent;
    friend class RequestTracker;
    friend class RequestTracker::Impl;
  };

  RequestTracker(Hooks& hooks);
  ~RequestTracker() noexcept(false);
  KJ_DISALLOW_COPY(RequestTracker);

  ActiveRequest startRequest();
  // Returns a new ActiveRequest, thereby bumping the count of active requests associated with the
  // RequestTracker. The ActiveRequest must be attached to the lifetime of the request such that we
  // destroy the ActiveRequest when the request is finished. On destruction, we decrement the count
  // of active requests associated with the RequestTracker, and if there are no more active requests
  // we call the `inactive()` hook.

private:
  class Impl : public kj::Refcounted {
  public:
    Impl(Hooks& hooksImpl);

  private:
    int activeRequests = 0;
    kj::Maybe<Hooks&> hooks;

    friend RequestTracker::ActiveRequest;
    friend RequestTracker::~RequestTracker() noexcept(false);
  };

  kj::Own<Impl> impl;
};

} // namespace workerd
