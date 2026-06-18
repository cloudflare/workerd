// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ofd-lock.h"

#include <kj/debug.h>
#include <kj/io.h>
#include <kj/test.h>

#if !_WIN32
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#endif

namespace workerd {
namespace {

#ifdef __linux__

kj::OwnFd openTempFile() {
  char path[] = "/tmp/ofd-lock-test-XXXXXX";
  int fd = mkstemp(path);
  KJ_SYSCALL(fd);
  unlink(path);
  return kj::OwnFd(fd);
}

KJ_TEST("exclusive lock conflicts with exclusive lock") {
  auto fd1 = openTempFile();

  // Open a second fd to the same file via /proc/self/fd.
  auto procPath = kj::str("/proc/self/fd/", fd1.get());
  int rawFd2;
  KJ_SYSCALL(rawFd2 = open(procPath.cStr(), O_RDWR));
  auto fd2 = kj::OwnFd(rawFd2);

  auto lock1 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd1, OfdLock::EXCLUSIVE));

  // Second exclusive lock on a different fd to the same file should fail.
  KJ_EXPECT(OfdLock::tryLock(fd2, OfdLock::EXCLUSIVE) == kj::none);
}

KJ_TEST("shared lock conflicts with exclusive lock") {
  auto fd1 = openTempFile();

  auto procPath = kj::str("/proc/self/fd/", fd1.get());
  int rawFd2;
  KJ_SYSCALL(rawFd2 = open(procPath.cStr(), O_RDWR));
  auto fd2 = kj::OwnFd(rawFd2);

  auto lock1 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd1, OfdLock::EXCLUSIVE));

  // Shared lock should also fail when exclusive is held.
  KJ_EXPECT(OfdLock::tryLock(fd2, OfdLock::SHARED) == kj::none);
}

KJ_TEST("shared locks do not conflict with each other") {
  auto fd1 = openTempFile();

  auto procPath = kj::str("/proc/self/fd/", fd1.get());
  int rawFd2;
  KJ_SYSCALL(rawFd2 = open(procPath.cStr(), O_RDWR));
  auto fd2 = kj::OwnFd(rawFd2);

  auto lock1 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd1, OfdLock::SHARED));
  auto lock2 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd2, OfdLock::SHARED));
  // Both shared locks held simultaneously — no conflict.
}

KJ_TEST("exclusive lock blocks while shared lock is held") {
  auto fd1 = openTempFile();

  auto procPath = kj::str("/proc/self/fd/", fd1.get());
  int rawFd2;
  KJ_SYSCALL(rawFd2 = open(procPath.cStr(), O_RDWR));
  auto fd2 = kj::OwnFd(rawFd2);

  auto lock1 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd1, OfdLock::SHARED));

  // Exclusive lock should fail when shared lock is held by a different fd.
  KJ_EXPECT(OfdLock::tryLock(fd2, OfdLock::EXCLUSIVE) == kj::none);
}

KJ_TEST("releasing lock allows re-acquisition") {
  auto fd1 = openTempFile();

  auto procPath = kj::str("/proc/self/fd/", fd1.get());
  int rawFd2;
  KJ_SYSCALL(rawFd2 = open(procPath.cStr(), O_RDWR));
  auto fd2 = kj::OwnFd(rawFd2);

  {
    auto lock1 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd1, OfdLock::EXCLUSIVE));
    // lock1 goes out of scope here, releasing the lock.
  }

  // Now fd2 should be able to acquire an exclusive lock.
  auto lock2 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd2, OfdLock::EXCLUSIVE));
}

KJ_TEST("move constructor transfers lock") {
  auto fd1 = openTempFile();

  auto procPath = kj::str("/proc/self/fd/", fd1.get());
  int rawFd2;
  KJ_SYSCALL(rawFd2 = open(procPath.cStr(), O_RDWR));
  auto fd2 = kj::OwnFd(rawFd2);

  auto lock1 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd1, OfdLock::EXCLUSIVE));

  // Move the lock.
  auto lock2 = kj::mv(lock1);

  // The lock should still be held (fd2 can't get it).
  KJ_EXPECT(OfdLock::tryLock(fd2, OfdLock::EXCLUSIVE) == kj::none);
}

KJ_TEST("move constructor releases on destruction of target") {
  auto fd1 = openTempFile();

  auto procPath = kj::str("/proc/self/fd/", fd1.get());
  int rawFd2;
  KJ_SYSCALL(rawFd2 = open(procPath.cStr(), O_RDWR));
  auto fd2 = kj::OwnFd(rawFd2);

  {
    auto lock1 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd1, OfdLock::EXCLUSIVE));
    auto lock2 = kj::mv(lock1);
    // lock2 goes out of scope, releasing the lock.
  }

  // fd2 should now succeed.
  auto lock3 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd2, OfdLock::EXCLUSIVE));
}

KJ_TEST("move assignment transfers lock") {
  auto fd1 = openTempFile();

  auto procPath = kj::str("/proc/self/fd/", fd1.get());
  int rawFd2;
  KJ_SYSCALL(rawFd2 = open(procPath.cStr(), O_RDWR));
  auto fd2 = kj::OwnFd(rawFd2);

  auto lock1 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd1, OfdLock::EXCLUSIVE));

  // Create a second lock on fd2... wait, we can't because fd1 already holds exclusive.
  // Instead, test move assignment by creating a dummy that gets replaced.
  auto fd3 = openTempFile();
  auto lock3 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd3, OfdLock::EXCLUSIVE));

  lock3 = kj::mv(lock1);  // Releases lock on fd3, takes lock from fd1.

  // fd2 still can't get the lock (lock on fd1 was transferred to lock3).
  KJ_EXPECT(OfdLock::tryLock(fd2, OfdLock::EXCLUSIVE) == kj::none);
}

KJ_TEST("same fd can re-lock (lock is per open file description)") {
  auto fd1 = openTempFile();

  // On the same fd, acquiring the same lock type again is a no-op re-lock, not a conflict.
  auto lock1 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd1, OfdLock::EXCLUSIVE));
  auto lock2 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd1, OfdLock::EXCLUSIVE));
  // Both succeed because OFD locks are per open file description, and this is the same fd.
}

KJ_TEST("verifyHeld succeeds when lock is held") {
  auto fd1 = openTempFile();
  auto lock1 = KJ_ASSERT_NONNULL(OfdLock::tryLock(fd1, OfdLock::EXCLUSIVE));
  // Should not throw.
  lock1.verifyHeld();
}

#else  // #if __linux__

KJ_TEST("dummy test: platform not supported") {}

#endif  // #if __linux__, #else

}  // namespace
}  // namespace workerd
