// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Negative fixtures for jsg-shared-visit: none may produce a diagnostic.

namespace kj {

class Refcounted {};
class AtomicRefcounted {};

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
  T& operator*() {
    return *ptr;
  }
  T* operator->() {
    return ptr;
  }

 private:
  T* ptr;
};

template <typename T>
class Vector {
 public:
  T* begin();
  T* end();
};

}  // namespace kj

namespace workerd::jsg {

class Value {};

class GcVisitor {
 public:
  template <typename... Args>
  void visit(Args&... args) {}
};

}  // namespace workerd::jsg

namespace jsg = workerd::jsg;

struct Chunk {
  jsg::Value value;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(value);
  }
};

struct SharedChunk: public kj::Refcounted {
  jsg::Value value;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(value);
  }
};

// N1: visiting through a kj::Own of a type that is not refcounted, which has one owner.
struct VisitsOwn {
  kj::Own<Chunk> chunk;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(*chunk);
  }
};

// N2: dereferencing a kj::Rc in a function that is not handed the visitor.
struct UsesRcElsewhere {
  kj::Rc<Chunk> chunk;

  jsg::Value& value() {
    return chunk->value;
  }
};

// N3: a suppressed site.
struct Suppressed {
  kj::Rc<Chunk> chunk;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(*chunk);  // NOLINT(jsg-shared-visit): only this owner ever visits the chunk.
  }
};

// N4: visiting a field of this object while a kj::Rc field is left unvisited.
struct VisitsOwnField {
  kj::Rc<Chunk> chunk;
  jsg::Value value;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(value);
  }
};

// N5: jsg's own implementation is left out.
namespace workerd::jsg {
struct InternalSharedVisit {
  kj::Rc<Chunk> chunk;

  void visitForGc(GcVisitor& visitor) {
    visitor.visit(*chunk);
  }
};
}  // namespace workerd::jsg
