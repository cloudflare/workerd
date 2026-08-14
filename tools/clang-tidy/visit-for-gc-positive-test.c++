// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Positive fixtures for the jsg-visit-for-gc check: every case below must
// produce a diagnostic. Self-contained stubs mirror the qualified names the
// check keys on (jsg::Ref, jsg::Name, kj::Own, kj::Rc, kj::Arc, ...).

namespace workerd::jsg {

class GcVisitor;

template <typename T>
class Ref {
 public:
  void visitForGc(GcVisitor& visitor) {}
};

class Name {
 public:
  void visitForGc(GcVisitor& visitor) {}
};

template <typename T>
class JsRef {
 public:
  void visitForGc(GcVisitor& visitor) {}
};

class JsValue {};

class GcVisitor {
 public:
  template <typename... Args>
  void visit(Args&&... args) {}
  template <typename T>
  void visitAll(T& collection) {}
};

class Object {
 public:
  void visitForGc(GcVisitor& visitor) {}
};

}  // namespace workerd::jsg

namespace kj {

template <typename T>
class Own {
 public:
  T& operator*() {
    return *ptr;
  }
  T* operator->() {
    return ptr;
  }
  T* get() {
    return ptr;
  }

 private:
  T* ptr = nullptr;
};

template <typename T>
class Rc {
 public:
  T& operator*() {
    return *ptr;
  }
  T* operator->() {
    return ptr;
  }
  T* get() {
    return ptr;
  }

 private:
  T* ptr = nullptr;
};

template <typename T>
class Arc {
 public:
  T& operator*() {
    return *ptr;
  }
  T* operator->() {
    return ptr;
  }

 private:
  T* ptr = nullptr;
};

}  // namespace kj

namespace jsg = workerd::jsg;

struct Widget: public jsg::Object {};

// Case 1: resource type with a visitForGc that misses a jsg::Ref field.
struct MissedRefField: public jsg::Object {
  jsg::Ref<Widget> visited;
  jsg::Ref<Widget> missed;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(visited);
  }
};

// Case 2: resource type with an unvisited jsg::Name field. jsg::Name holds a
// V8Ref<v8::Symbol> and is directly visitable.
struct MissedNameField: public jsg::Object {
  jsg::Name name;

  void visitForGc(jsg::GcVisitor& visitor) {}
};

struct OwnedState {
  jsg::Ref<Widget> ref;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(ref);
  }
};

// Case 3: visiting through a dereferenced kj::Own. Ownership is a traversal
// barrier: the owned object is not reliably re-visited after a move, so a
// visited handle can be left weak and collected.
struct VisitThroughOwnDeref: public jsg::Object {
  kj::Own<OwnedState> state;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(*state);
  }
};

struct SharedState {
  jsg::JsRef<jsg::JsValue> value;
};

// Case 4: reaching a field through kj::Rc's operator->.
struct VisitThroughRcArrow: public jsg::Object {
  kj::Rc<SharedState> shared;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(shared->value);
  }
};

// Case 5: reaching through kj::Arc in a non-first argument position.
struct VisitThroughArcDeref: public jsg::Object {
  jsg::Ref<Widget> direct;
  kj::Arc<OwnedState> state;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(direct, *state);
  }
};

// Case 6: reaching through kj::Own via get().
struct VisitThroughOwnGet: public jsg::Object {
  kj::Own<SharedState> owned;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(owned.get()->value);
  }
};

// Case 7: a direct visitForGc call through a barrier crosses it just the same
// as visitor.visit(*owned).
struct DirectVisitCallThroughOwn: public jsg::Object {
  kj::Own<OwnedState> state;

  void visitForGc(jsg::GcVisitor& visitor) {
    state->visitForGc(visitor);
  }
};
