// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Positive fixtures for the jsg-visit-for-gc check: every case below must
// produce exactly one diagnostic, and visit-for-gc-test.sh asserts the exact
// total. Self-contained stubs mirror the qualified names the check keys on.

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

// Case P2: resource type with no visitForGc of its own; the framework
// dispatches to jsg::Object's empty default and misses the field.
struct NoVisitMethod: public jsg::Object {
  jsg::Ref<Widget> orphaned;
};

// Case P3: unvisited jsg::Name field. This locks CURRENT behavior: the check
// lists jsg::Name as a visitable leaf type.
struct MissedNameField: public jsg::Object {
  jsg::Name name;

  void visitForGc(jsg::GcVisitor& visitor) {}
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
