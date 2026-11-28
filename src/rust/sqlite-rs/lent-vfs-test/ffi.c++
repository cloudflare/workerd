// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <kj/time.h>

namespace workerd::rust::sqlite_rs::lent_vfs_test {

namespace {

thread_local size_t ownersAlive = 0;

}  // namespace

VfsOwner::VfsOwner(): directory(kj::newInMemoryDirectory(kj::nullClock())), vfs(*directory) {
  ++ownersAlive;
}

VfsOwner::~VfsOwner() noexcept(false) {
  --ownersAlive;
}

kj::Own<SqliteVfs> vfs_lent_by_new_owner() {
  auto owner = kj::refcounted<VfsOwner>();
  return kj::Own<SqliteVfs>(&owner->vfs, kj::NullDisposer::instance).attach(kj::mv(owner));
}

size_t vfs_owners_alive() {
  return ownersAlive;
}

}  // namespace workerd::rust::sqlite_rs::lent_vfs_test
