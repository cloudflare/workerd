// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Negative fixtures for jsg-traced-own: none may produce a diagnostic.

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

// N1: visiting through a TracedOwn.
struct VisitsTracedOwn {
  jsg::TracedOwn<Request> request;
  kj::Vector<jsg::TracedOwn<Request>> requests;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(request);
    visitor.visitAll(requests);
  }
};

// N2: dereferencing an Own in a function that is not handed the visitor.
struct UsesOwnElsewhere {
  kj::Own<Request> request;

  Request& touch() {
    return *request;
  }
};

// N3: a kj::Rc is shared, so moving it does not move the object out of its other owners.
struct VisitsThroughRc {
  kj::Rc<Request> request;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(*request);
  }
};

// N4: moving one TracedOwn, or a kj::Maybe of one, moves the element itself.
struct MovesOneElement {
  jsg::TracedOwn<Request> request;
  kj::Maybe<jsg::TracedOwn<Request>> maybeRequest;

  void take() {
    auto one = kj::mv(request);
    auto maybe = kj::mv(maybeRequest);
  }
};

// N5: moving a local container, whose elements were moved into it one at a time.
kj::Vector<jsg::TracedOwn<Request>> returnsLocal(kj::Vector<jsg::TracedOwn<Request>>& source) {
  kj::Vector<jsg::TracedOwn<Request>> result;
  return kj::mv(result);
}

// N6: a suppressed site.
struct Suppressed {
  kj::Own<Request> request;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(*request);  // NOLINT(jsg-traced-own): the Own never leaves this object.
  }
};

// N7: moving a container whose struct elements hold no TracedOwn.
struct MovesPlainStructElements {
  struct Event {
    kj::Maybe<kj::Own<Request>> request;
  };
  workerd::RingBuffer<Event, 8> events;

  void drop() {
    auto all = kj::mv(events);
  }
};

// N8: moving a container of pointers to objects that hold a TracedOwn.
struct HoldsTracedOwn {
  jsg::TracedOwn<Request> request;
};
struct MovesPointers {
  kj::Vector<kj::Own<HoldsTracedOwn>> owners;

  void drop() {
    auto all = kj::mv(owners);
  }
};
