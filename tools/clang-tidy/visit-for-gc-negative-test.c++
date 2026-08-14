// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Negative fixtures for the jsg-visit-for-gc check: nothing below may produce
// a diagnostic. Self-contained stubs mirror the qualified names the check
// keys on (jsg::Ref, jsg::Name, kj::Own, kj::Rc, kj::Arc, ...).

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
class Maybe {};

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

// Case 1: all visitable fields visited directly.
struct AllVisited: public jsg::Object {
  jsg::Ref<Widget> ref;
  kj::Maybe<jsg::Ref<Widget>> maybeRef;
  jsg::Name name;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(ref, maybeRef, name);
  }
};

struct OwnedState {
  jsg::Ref<Widget> ref;
};

struct SharedState {
  jsg::JsRef<jsg::JsValue> value;
};

// Case 2: JSG handles behind kj::Own/kj::Rc/kj::Arc are ownership barriers.
// They are held as strong (unvisited) roots; not visiting them is correct and
// must not be diagnosed, including when wrapped in kj::Maybe.
struct BarriersNotVisited: public jsg::Object {
  kj::Own<OwnedState> owned;
  kj::Rc<SharedState> shared;
  kj::Arc<SharedState> atomicShared;
  kj::Maybe<kj::Own<OwnedState>> maybeOwned;

  void visitForGc(jsg::GcVisitor& visitor) {}
};

struct NestedState {
  jsg::Ref<Widget> func;
};

// Case 3: a parent's visitForGc may reach into a directly-held (non-barrier)
// nested struct member.
struct ParentReachesNested: public jsg::Object {
  NestedState state;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(state.func);
  }
};

// Case 4: non-barrier deref arguments are fine (e.g. plain struct accessor
// results); only kj::Own/kj::Rc/kj::Arc derefs are barriers.
struct HolderByValue {
  NestedState inner;
  NestedState& get() {
    return inner;
  }
};

struct VisitThroughPlainAccessor: public jsg::Object {
  HolderByValue holder;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(holder.get().func);
  }
};

struct DelegatingState {
  jsg::Ref<Widget> func;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(func);
  }
};

// Case 5: delegating to a directly-held member's visitForGc is fine; only
// delegation through a barrier is diagnosed.
struct DelegateToDirectMember: public jsg::Object {
  DelegatingState state;

  void visitForGc(jsg::GcVisitor& visitor) {
    state.visitForGc(visitor);
  }
};
