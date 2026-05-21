// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/memory.h>

namespace workerd {

class OfdLock {
  // RAII holder for a lock on an open file description. Releasing this object releases the lock.
  //
  // Uses Linux OFD (Open File Description) locks via fcntl(F_OFD_SETLK). OFD locks are per-fd
  // rather than per-process, which is essential since a single workerd process may manage many
  // Durable Objects concurrently.
 public:
  enum Type { SHARED, EXCLUSIVE };

  // Try to acquire a lock (non-blocking). Returns kj::none if the lock is held in a conflicting
  // mode by another open file description.
  //
  // On non-Linux platforms, fails at runtime with KJ_FAIL_REQUIRE rather than a compile error,
  // so non-Linux builds still compile when cluster mode is unused.
  static kj::Maybe<OfdLock> tryLock(int fd, Type type);

  // Verify that a lock that this open file description was previously granted is still held.
  // Specifically, this is meant to check for NFSv4 lease loss. Implemented as a no-op
  // F_OFD_SETLK of the same lock type on the same fd: if the underlying NFSv4 lock stateid
  // has been invalidated (lease lost), the Linux NFS client surfaces this as EIO and this
  // function throws; if the stateid is still valid, the kernel treats the re-lock as a
  // no-op and we return cleanly.
  void verifyHeld();

  OfdLock(OfdLock&& other);
  OfdLock& operator=(OfdLock&& other);
  KJ_DISALLOW_COPY(OfdLock);

  ~OfdLock();  // releases via F_OFD_SETLK with F_UNLCK

 private:
  int fd;  // -1 after move
  Type type;
  explicit OfdLock(int fd, Type type);
};

}  // namespace workerd
