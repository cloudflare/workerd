// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Positive fixtures for jsg-traced-own: each case must produce exactly one diagnostic;
// traced-own-test.sh asserts the exact total.

namespace kj {

template <typename T>
T&& mv(T& value) {
  return static_cast<T&&>(value);
}

template <typename T>
class Own {
 public:
  T& operator*() const {
    return *ptr;
  }
  T* operator->() const {
    return ptr;
  }

 private:
  T* ptr;
};

template <typename T>
class Rc {
 public:
  T& operator*() const {
    return *ptr;
  }
  T* operator->() const {
    return ptr;
  }

 private:
  T* ptr;
};

template <typename T>
class Maybe {};

template <typename... T>
class OneOf {
  alignas(T...) char storage[16];
};

template <typename T>
class Vector {
 public:
  T* begin();
  T* end();
};

}  // namespace kj

namespace workerd {

template <typename T, int N>
class RingBuffer {};

namespace jsg {

class Value {};

class GcVisitor {
 public:
  template <typename... Args>
  void visit(Args&... args) {}
  template <typename T>
  void visitAll(T& collection) {}
};

template <typename T>
class TracedOwn {
 public:
  TracedOwn(kj::Own<T> inner);
  T* operator->();
  void visitForGc(GcVisitor& visitor);
};

}  // namespace jsg
}  // namespace workerd

namespace jsg = workerd::jsg;

struct Request {
  jsg::Value resolver;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(resolver);
  }
};

// P1: visiting the object behind an Own.
struct VisitsDeref {
  kj::Own<Request> request;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(*request);
  }
};

// P2: visiting a field reached through an Own.
struct VisitsArrowField {
  kj::Own<Request> request;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(request->resolver);
  }
};

// P3: visiting the elements of a container of Owns.
struct VisitsOwnElements {
  kj::Vector<kj::Own<Request>> requests;

  void visitForGc(jsg::GcVisitor& visitor) {
    for (auto& request: requests) {
      visitor.visit(request->resolver);
    }
  }
};

// P4: forwarding visitForGc through an Own.
struct ForwardsThroughOwn {
  kj::Own<Request> request;

  void visitForGc(jsg::GcVisitor& visitor) {
    request->visitForGc(visitor);
  }
};

// P5: moving a whole kj::Vector of TracedOwns out of a field.
struct MovesVector {
  kj::Vector<jsg::TracedOwn<Request>> requests;

  kj::Vector<jsg::TracedOwn<Request>> takeAll() {
    return kj::mv(requests);
  }
};

// P6: moving a whole RingBuffer of TracedOwns out of a nested field.
struct MovesRingBuffer {
  struct Ready {
    workerd::RingBuffer<jsg::TracedOwn<Request>, 8> requests;
  } ready;

  void drop() {
    auto all = kj::mv(ready.requests);
  }
};

// P7: visiting through an Own in a helper that visitForGc calls.
struct VisitsInHelper {
  kj::Own<Request> request;

  void visitPending(jsg::GcVisitor& visitor) {
    visitor.visit(request->resolver);  // P7
  }
};

// P8: moving a whole RingBuffer whose struct elements hold a TracedOwn inside a kj::OneOf.
struct MovesStructElements {
  struct Event {
    kj::OneOf<int, kj::Maybe<jsg::TracedOwn<Request>>> request;
  };
  workerd::RingBuffer<Event, 8> events;

  void drop() {
    auto all = kj::mv(events);
  }
};
