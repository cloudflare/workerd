// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The Rust implementation of the virtual file system (io/vfs) behind the worker-fs.h API. When
// the RUST_VFS autogate is enabled, worker-fs.c++'s factory functions create its nodes and file
// systems instead of the C++ ones, so callers don't change. Its nodes hold only nodes of their
// own kind: adding a C++ node to a Rust directory fails.

#include <workerd/io/vfs/ffi/ffi.rs.h>
#include <workerd/io/worker-fs.h>
#include <workerd/util/strong-bool.h>

#include <kj/debug.h>

namespace workerd {

// Logs a line written to stdout or stderr through console.log, with the isolate's prefix for the
// stream. Shared by both implementations.
void writeStdio(jsg::Lock& js, VirtualFileSystem::Stdio type, kj::ArrayPtr<const kj::byte> bytes);

// What worker-fs.h's SymbolicLink, SymbolicLinkRecursionGuardScope and TmpDirStoreScope hold for
// the Rust implementation.
class RustSymbolicLink final {
 public:
  explicit RustSymbolicLink(::rust::Box<vfs::RsSymbolicLink> inner): inner(kj::mv(inner)) {}

  const vfs::RsSymbolicLink& get() const {
    return *inner;
  }

 private:
  ::rust::Box<vfs::RsSymbolicLink> inner;
};

// Rust holds the guard mutably for the whole of a check or resolution through it, which may call
// C++, such as a lazy directory's loader. That C++ must not check or resolve a link through the
// same guard, which would be a second mutable borrow, so the guard is lent to one call at a time
// and a call while it is lent throws.
class RustLinkGuard final {
 public:
  template <typename Func>
  auto lend(Func&& func) {
    KJ_REQUIRE(!lent,
        "a symbolic link was checked or resolved while a link of the Rust file system resolved "
        "through the same guard");
    lent = true;
    KJ_DEFER(lent = false);
    return func(*inner);
  }

 private:
  ::rust::Box<vfs::RsLinkGuard> inner = vfs::link_guard_new();
  bool lent = false;
};

class RustTmpStore final {
 public:
  RustTmpStore(::rust::Box<vfs::RsTmpStore> store, kj::Maybe<::rust::Box<vfs::RsTmpScope>> scope)
      : scope(kj::mv(scope)),
        store(kj::mv(store)) {}

  const vfs::RsTmpStore& get() const {
    return *store;
  }

 private:
  // Keeps the store current on this thread, if it is.
  kj::Maybe<::rust::Box<vfs::RsTmpScope>> scope;
  ::rust::Box<vfs::RsTmpStore> store;
};

namespace vfs {

// Whether the RUST_VFS autogate chooses the Rust implementation.
bool isEnabled();

kj::Rc<File> newWritableFile(jsg::Lock& js, kj::Maybe<uint32_t> size) KJ_WARN_UNUSED_RESULT;
kj::Rc<File> newReadableFile(kj::ArrayPtr<const kj::byte> data) KJ_WARN_UNUSED_RESULT;
kj::Rc<File> newReadableFile(kj::Array<const kj::byte> data) KJ_WARN_UNUSED_RESULT;
kj::Rc<File> newDevNull() KJ_WARN_UNUSED_RESULT;
kj::Rc<File> newDevZero() KJ_WARN_UNUSED_RESULT;
kj::Rc<File> newDevFull() KJ_WARN_UNUSED_RESULT;
kj::Rc<File> newDevRandom() KJ_WARN_UNUSED_RESULT;

kj::Rc<Directory> newWritableDirectory() KJ_WARN_UNUSED_RESULT;
kj::Rc<Directory> newWritableDirectory(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;
kj::Rc<Directory> newTmpDirectory() KJ_WARN_UNUSED_RESULT;
kj::Rc<Directory> newLazyDirectory(kj::Function<kj::Rc<Directory>()> func) KJ_WARN_UNUSED_RESULT;
// Directory::Builder::finish().
kj::Rc<Directory> finishBuilder(Directory::Builder::Map entries) KJ_WARN_UNUSED_RESULT;

kj::Own<VirtualFileSystem> newVirtualFileSystem(kj::Own<FsMap> fsMap,
    kj::Rc<Directory> root,
    kj::Own<VirtualFileSystem::Observer> observer) KJ_WARN_UNUSED_RESULT;

// A link to target within root, or none if root belongs to the C++ implementation.
kj::Maybe<kj::Own<RustSymbolicLink>> tryNewSymbolicLink(const Directory& root,
    kj::PathPtr target,
    kj::Maybe<kj::StringPtr> rawTarget) KJ_WARN_UNUSED_RESULT;
// The target as written, if the link keeps one.
kj::Maybe<kj::String> getLinkRawTarget(const RustSymbolicLink& link) KJ_WARN_UNUSED_RESULT;

// The members of SymbolicLink for a link of the Rust implementation.
kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>> resolveLink(
    jsg::Lock& js, const RustSymbolicLink& link) KJ_WARN_UNUSED_RESULT;
// Valid while the link lives, which may be longer than `link`.
kj::StringPtr getLinkUniqueId(jsg::Lock& js, const RustSymbolicLink& link) KJ_WARN_UNUSED_RESULT;
void countLinkTowardsIsolateLimit(jsg::Lock& js, const RustSymbolicLink& link);
bool isSameLink(const RustSymbolicLink& link, const RustSymbolicLink& other) KJ_WARN_UNUSED_RESULT;

// SymbolicLinkRecursionGuardScope for links of the Rust implementation.
kj::Own<RustLinkGuard> newLinkGuard() KJ_WARN_UNUSED_RESULT;
kj::Maybe<FsError> checkLink(
    RustLinkGuard& guard, const RustSymbolicLink& link) KJ_WARN_UNUSED_RESULT;

WD_STRONG_BOOL(MakeCurrent);

// The store behind a TmpDirStoreScope. With MakeCurrent::YES, the store is current on this thread
// until it is destroyed.
kj::Own<RustTmpStore> newTmpStore(MakeCurrent makeCurrent) KJ_WARN_UNUSED_RESULT;
kj::Rc<Directory> getTmpStoreDirectory(const RustTmpStore& store) KJ_WARN_UNUSED_RESULT;

}  // namespace vfs
}  // namespace workerd
