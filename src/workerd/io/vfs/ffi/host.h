// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ side of what the Rust VFS calls back into. Each class is abstract: the bridge calls only
// virtual methods, which //src/workerd/io implements (worker-fs-rust.c++). That way this header,
// and the bridge that includes it, depend on nothing in workerd, while //src/workerd/io depends on
// the bridge.

#include <rust/cxx.h>

#include <cstddef>
#include <cstdint>

namespace workerd::vfs {

struct RsDirectory;
struct RsLoaded;
struct RsMicrotask;
struct RsTmpStore;

// What the file system needs from the runtime for one operation: the Rust `VfsHost`. An instance
// exists only while the isolate lock is held.
class VfsHost {
 public:
  // The `v8::Isolate*` whose lock is held, as an address, for charging memory to it.
  virtual size_t isolate() const = 0;
  virtual size_t blobSizeLimit() const = 0;
  // Returns false outside a request, where there is no source of randomness.
  virtual bool fillRandom(::rust::Slice<uint8_t> buffer) const = 0;
  // Returns an empty string outside a request.
  virtual ::rust::String randomUuid() const = 0;
  virtual bool hasIoContext() const = 0;
  // Logs a line to stdout (1) or stderr (2). Returns false if console.log threw; the host keeps
  // the exception and throws it once the file system returns.
  virtual bool logStdio(uint8_t stdio, ::rust::Slice<const uint8_t> line) const = 0;
  virtual void enqueueMicrotask(::rust::Box<RsMicrotask> task) const = 0;
  // Whether the current request has a temporary directory store, and a reference to it.
  virtual bool hasIoContextTmpStore() const = 0;
  virtual ::rust::Box<RsTmpStore> ioContextTmpStore() const = 0;

 protected:
  ~VfsHost() noexcept(false) = default;
};

// C++ `VirtualFileSystem::Observer`.
class FdObserver {
 public:
  virtual ~FdObserver() noexcept(false) = default;
  virtual void onOpen(size_t openFds, size_t totalFds) const = 0;
  virtual void onClose(size_t openFds, size_t totalFds) const = 0;
  virtual void onMaxFds(size_t openFds) const = 0;
};

// Bytes that C++ owns, such as a kj::Array, handed to Rust for a read-only file to keep.
class OwnedBytes {
 public:
  virtual ~OwnedBytes() noexcept(false) = default;
  // The same bytes on every call, for as long as this object lives.
  virtual ::rust::Slice<const uint8_t> bytes() const = 0;
};

// The C++ jsg::MemoryTracker of a heap snapshot in progress: the Rust `MemoryTracker`. Names
// are NUL-terminated literals, which the snapshot may keep.
class SnapshotTracker {
 public:
  virtual bool pushNode(::rust::Slice<const uint8_t> edge,
      size_t id,
      ::rust::Slice<const uint8_t> name,
      size_t selfSize) = 0;
  virtual void popNode() = 0;
  virtual void trackBytes(
      ::rust::Slice<const uint8_t> edge, ::rust::Slice<const uint8_t> name, size_t size) = 0;

 protected:
  ~SnapshotTracker() noexcept(false) = default;
};

// The function behind a lazy directory, called until it succeeds. Puts the directory in `loaded`
// and returns true, or returns false after giving what went wrong to the host of the operation
// that called it, which throws it once the file system returns.
class LazyLoader {
 public:
  virtual ~LazyLoader() noexcept(false) = default;
  virtual bool load(RsLoaded& loaded) = 0;
};

}  // namespace workerd::vfs
