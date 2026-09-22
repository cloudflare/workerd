// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Negative fixtures for jsg-visit-for-gc: nothing below may produce a
// diagnostic.

namespace workerd::jsg {

class GcVisitor;

template <typename T>
class Ref {
 public:
  void visitForGc(GcVisitor& visitor) {}
};

// Mirrors the real jsg::Name: visitForGc is private.
class Name {
 private:
  void visitForGc(GcVisitor& visitor) {}
};

template <typename T>
class Promise {
 public:
  class Resolver {
   public:
    void visitForGc(GcVisitor& visitor) {}
  };

  void visitForGc(GcVisitor& visitor) {}
};

template <typename T>
class Generator {
 public:
  void visitForGc(GcVisitor& visitor) {}
};

template <typename T>
class AsyncGenerator {
 public:
  void visitForGc(GcVisitor& visitor) {}
};

template <typename T>
class Sequence {};

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

// Case N2: a parent's visitForGc may reach into a directly-held nested
// struct member.
struct ParentReachesNested: public jsg::Object {
  NestedState state;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(state.func);
  }
};

// Case N3: a plain standalone holder is not demanded against.
struct StandalonePlainHolder {
  jsg::Ref<Widget> strongRoot;
};

// Case N4: jsg::Name is not demanded; its private visitForGc makes visiting
// impossible, and the symbol handle is a strong root.
struct UnvisitedNameField: public jsg::Object {
  jsg::Name name;

  void visitForGc(jsg::GcVisitor& visitor) {}
};

// Case N5: KNOWN BLIND SPOT, locked as current behavior: any mention of the
// field inside the body counts as a visit, even a comparison.
struct MentionOnlyCountsAsVisit: public jsg::Object {
  kj::Maybe<jsg::Ref<Widget>> mentioned;

  void visitForGc(jsg::GcVisitor& visitor) {
    if (mentioned == nullptr) {
      return;
    }
  }
};

// Case N6: visited resolver fields are accepted.
struct VisitedResolver: public jsg::Object {
  jsg::Promise<int>::Resolver resolver;
  kj::Maybe<jsg::Promise<int>::Resolver> maybeResolver;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(resolver, maybeResolver);
  }
};

// Case N7: visited generators; Sequence via visitAll; non-visitable element
// Sequence held strong.
struct GeneratorAndSequence: public jsg::Object {
  jsg::Generator<int> gen;
  jsg::Sequence<int> plainSeq;
  jsg::Sequence<jsg::Ref<Widget>> refSeq;
  jsg::AsyncGenerator<int> asyncGen;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(gen, asyncGen);
    visitor.visitAll(refSeq);
  }
};
