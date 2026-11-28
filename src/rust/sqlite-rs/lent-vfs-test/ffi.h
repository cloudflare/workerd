// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/rust/sqlite-rs/ffi.h>

#include <kj/filesystem.h>
#include <kj/refcount.h>

namespace workerd::rust::sqlite_rs::lent_vfs_test {

// Owns a VFS the way C++ storage does (the factory's `ActorStorage::Impl`): refcounted, with the
// VFS as a member, lent to Rust as a non-owning `kj::Own` that keeps a reference to its owner.
class VfsOwner final: public kj::Refcounted {
 public:
  VfsOwner();
  ~VfsOwner() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(VfsOwner);

  // Declared before `vfs`, which uses it.
  kj::Own<const kj::Directory> directory;
  SqliteVfs vfs;
};

// A new owner's VFS, through a `kj::Own` that holds the only reference to the owner.
kj::Own<SqliteVfs> vfs_lent_by_new_owner();
// How many owners on this thread are alive.
size_t vfs_owners_alive();

}  // namespace workerd::rust::sqlite_rs::lent_vfs_test
