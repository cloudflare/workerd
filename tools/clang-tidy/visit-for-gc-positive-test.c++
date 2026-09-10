// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Positive fixtures for jsg-visit-for-gc: each case must produce exactly one
// diagnostic; visit-for-gc-test.sh asserts the exact total.

namespace workerd::jsg {

class GcVisitor;

template <typename T>
class Ref {
 public:
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

// Case P1: visitForGc exists but misses a jsg::Ref field.
struct MissedRefField: public jsg::Object {
  jsg::Ref<Widget> visited;
  jsg::Ref<Widget> missed;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(visited);
  }
};

// Case P2: no visitForGc of its own; jsg::Object's empty default misses the
// field.
struct NoVisitMethod: public jsg::Object {
  jsg::Ref<Widget> orphaned;
};

// Case P4: unvisited kj::Maybe<jsg::Ref<T>> field (FirstArg container).
struct MissedMaybeRef: public jsg::Object {
  kj::Maybe<jsg::Ref<Widget>> maybeRef;

  void visitForGc(jsg::GcVisitor& visitor) {}
};

// Case P5: unvisited kj::OneOf with a visitable alternative (AnyArg
// container).
struct MissedOneOf: public jsg::Object {
  kj::OneOf<int, jsg::Ref<Widget>> stateful;

  void visitForGc(jsg::GcVisitor& visitor) {}
};

// Case P6: unvisited jsg::Promise<T>::Resolver field.
struct MissedResolver: public jsg::Object {
  jsg::Promise<int>::Resolver resolver;

  void visitForGc(jsg::GcVisitor& visitor) {}
};

// Case P7: unvisited kj::Maybe<jsg::Promise<T>::Resolver> field.
struct MissedMaybeResolver: public jsg::Object {
  kj::Maybe<jsg::Promise<int>::Resolver> maybeResolver;

  void visitForGc(jsg::GcVisitor& visitor) {}
};

// Case P8: unvisited jsg::Generator<T> field.
struct MissedGenerator: public jsg::Object {
  jsg::Generator<int> gen;

  void visitForGc(jsg::GcVisitor& visitor) {}
};

// Case P9: unvisited jsg::AsyncGenerator<T> field.
struct MissedAsyncGenerator: public jsg::Object {
  jsg::AsyncGenerator<int> gen;

  void visitForGc(jsg::GcVisitor& visitor) {}
};

// Case P10: unvisited jsg::Sequence with a visitable element type.
struct MissedSequence: public jsg::Object {
  jsg::Sequence<jsg::Ref<Widget>> seq;

  void visitForGc(jsg::GcVisitor& visitor) {}
};
