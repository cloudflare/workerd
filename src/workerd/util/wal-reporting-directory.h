// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/filesystem.h>
#include <kj/refcount.h>

namespace workerd {

// Receives each write that SQLite makes to a WAL through a directory from
// `newWalReportingDirectory()`.
class WalObserver: public kj::AtomicRefcounted {
 public:
  // `data` was written at `offset` to the WAL at `path`, relative to the directory. Called once
  // the write succeeded, on the thread that made it. An exception fails the write as SQLite sees
  // it, though the data is already in the file.
  virtual void walWrite(
      kj::StringPtr path, uint64_t offset, kj::ArrayPtr<const kj::byte> data) const = 0;
};

// `inner`, with each write to a WAL (a file whose name ends in `-wal`, as SQLite names a
// database's WAL) reported to `observer` once it succeeds. The directory has no fd, so a
// `SqliteDatabase::Vfs` over it uses the KJ VFS on every platform, and SQLite's writes go through
// the directory's files. Its clones and the WALs it opens share `observer`.
kj::Own<const kj::Directory> newWalReportingDirectory(
    kj::Own<const kj::Directory> inner, kj::Arc<WalObserver> observer);

}  // namespace workerd
