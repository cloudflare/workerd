// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Positive fixtures for jsg-shared-visit: each case must produce exactly one diagnostic;
// shared-visit-test.sh asserts the exact total.

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
class Arc {
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
  template <typename T>
  void visitAll(T& collection) {}
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

// P1: visiting the object behind a kj::Rc.
struct VisitsRcDeref {
  kj::Rc<Chunk> chunk;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(*chunk);  // P1
  }
};

// P2: visiting a field reached through a kj::Rc.
struct VisitsRcField {
  kj::Rc<Chunk> chunk;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(chunk->value);  // P2
  }
};

// P3: forwarding visitForGc through a kj::Rc.
struct ForwardsThroughRc {
  kj::Rc<Chunk> chunk;

  void visitForGc(jsg::GcVisitor& visitor) {
    chunk->visitForGc(visitor);  // P3
  }
};

// P4: visiting through a kj::Own of a refcounted type, which kj::addRef() can share.
struct VisitsRefcountedOwn {
  kj::Own<SharedChunk> chunk;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(*chunk);  // P4
  }
};

// P5: visiting the elements of a container of kj::Rc in a helper handed the visitor.
struct VisitsRcElements {
  kj::Vector<kj::Rc<Chunk>> chunks;

  void visitChunks(jsg::GcVisitor& visitor) {
    for (auto& chunk: chunks) {
      visitor.visit(chunk->value);  // P5
    }
  }
};

struct AtomicSharedChunk: public kj::AtomicRefcounted {
  jsg::Value value;
};

// P6: visiting the object behind a kj::Arc.
struct VisitsArcDeref {
  kj::Arc<Chunk> chunk;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(*chunk);  // P6
  }
};

// P7: visiting through a kj::Own of an atomically refcounted type.
struct VisitsAtomicRefcountedOwn {
  kj::Own<AtomicSharedChunk> chunk;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(chunk->value);  // P7
  }
};

// P8: visitAll through a kj::Rc.
struct VisitsAllThroughRc {
  kj::Rc<Chunk> chunk;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visitAll(chunk->value);  // P8
  }
};
