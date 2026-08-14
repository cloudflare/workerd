// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Negative fixtures for the jsg-visit-for-gc check: nothing below may produce
// a diagnostic. Self-contained stubs mirror the qualified names the check
// keys on.

namespace workerd::jsg {

class GcVisitor;

template <typename T>
class Ref {
 public:
  void visitForGc(GcVisitor& visitor) {}
};

// Mirrors the real jsg::Name: visitForGc is private (friend-only), so no
// holder can visit a Name.
class Name {
 private:
  void visitForGc(GcVisitor& visitor) {}
};

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

// Records inside namespace jsg are framework internals and are skipped.
class FrameworkInternal {
 public:
  Ref<FrameworkInternal> unvisited;
};

}  // namespace workerd::jsg

namespace kj {

template <typename T>
class Maybe {
 public:
  bool operator==(decltype(nullptr)) const {
    return true;
  }
};

template <typename... T>
class OneOf {};

}  // namespace kj

namespace jsg = workerd::jsg;

struct Widget: public jsg::Object {};

// Case N1: every visitable field is visited.
struct AllVisited: public jsg::Object {
  jsg::Ref<Widget> ref;
  kj::Maybe<jsg::Ref<Widget>> maybeRef;
  kj::OneOf<int, jsg::Ref<Widget>> stateful;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(ref, maybeRef, stateful);
  }
};

struct NestedState {
  jsg::Ref<Widget> func;
};

// Case N2: a parent's visitForGc reaches into a directly-held nested struct
// member; the nested struct needs no visitForGc of its own.
struct ParentReachesNested: public jsg::Object {
  NestedState state;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(state.func);
  }
};

// Case N3: a plain standalone holder (no jsg::Object base, no visitForGc, not
// used as a field of any record in this TU) is not demanded against.
struct StandalonePlainHolder {
  jsg::Ref<Widget> strongRoot;
};

// Case N4: jsg::Name is not a demanded type. Its visitForGc is private
// (friend-only), so no holder can visit it; the symbol handle is held as a
// strong root for the holder's lifetime, and a v8::Symbol cannot form a
// JS<->C++ cycle, so visitation is never required.
struct UnvisitedNameField: public jsg::Object {
  jsg::Name name;

  void visitForGc(jsg::GcVisitor& visitor) {}
};

// Case N5: KNOWN BLIND SPOT, locked as current behavior. Any MemberExpr
// naming the field inside the visitForGc body counts as a "visit" — including
// a mere comparison. The check does not verify the field is an argument of
// GcVisitor::visit.
struct MentionOnlyCountsAsVisit: public jsg::Object {
  kj::Maybe<jsg::Ref<Widget>> mentioned;

  void visitForGc(jsg::GcVisitor& visitor) {
    if (mentioned == nullptr) {
      return;
    }
  }
};
