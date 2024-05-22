#pragma once

#include <kj/map.h>

namespace v8 {
  class Isolate;
}

namespace workerd::jsg {

// The UnsettledPromiseTracker is a local-dev utility that tracks all promises
// created in an isolate and will generate a report of all promises that are
// unsettled when `report()` is called. This is useful for debugging cases where
// promises may be hanging to know exactly where they were created.
//
// Note that this uses the v8 Promise Hooks API under the covers. If we add more
// uses of the promise hooks API, the implementation of this will need to be
// refactored a bit as the design of that API allows only one set of hooks to
// be installed on an isolate at a time.
//
// Only ONE instance of the UnsettledPromiseTracker should be created at a time
// for any single isolate.
class UnsettledPromiseTracker final {
public:
  UnsettledPromiseTracker(v8::Isolate* isolate);
  ~UnsettledPromiseTracker() noexcept(false);

  KJ_DISALLOW_COPY_AND_MOVE(UnsettledPromiseTracker);

  kj::String report();

  inline size_t size() const { return promises_.size(); }
  inline void reset() { promises_.clear(); }

private:
  v8::Isolate* isolate;

  // We don't want to maintain strong references to the promises themselves so
  // here we are going to maintain a table of the promise id hash and the serialized
  // stack identifying where the promise was created.
  kj::HashMap<int, kj::String> promises_;
};

}  // namespace workerd::jsg
