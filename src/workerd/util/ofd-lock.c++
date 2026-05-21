// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ofd-lock.h"

#include <kj/debug.h>

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#endif

namespace workerd {

#ifdef __linux__

namespace {

struct flock makeFlock(short type) {
  struct flock fl = {};
  fl.l_type = type;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;  // Lock the entire file.
  return fl;
}

}  // namespace

kj::Maybe<OfdLock> OfdLock::tryLock(int fd, Type type) {
  auto fl = makeFlock(type == SHARED ? F_RDLCK : F_WRLCK);
  if (fcntl(fd, F_OFD_SETLK, &fl) == -1) {
    int err = errno;
    if (err == EAGAIN || err == EACCES) {
      // Lock is held by another open file description.
      return kj::none;
    }
    KJ_FAIL_SYSCALL("fcntl(F_OFD_SETLK)", err);
  }
  return OfdLock(fd, type);
}

void OfdLock::verifyHeld() {
  KJ_REQUIRE(fd >= 0, "OfdLock has been moved away");
  // Re-lock with the same type. If the underlying NFSv4 lock stateid has been invalidated
  // (lease lost), the Linux NFS client surfaces this as EIO. If the stateid is still valid,
  // the kernel treats the re-lock as a no-op.
  auto fl = makeFlock(type == SHARED ? F_RDLCK : F_WRLCK);
  KJ_SYSCALL(fcntl(fd, F_OFD_SETLK, &fl), "NFS lease may have been lost");
}

OfdLock::OfdLock(int fd, Type type): fd(fd), type(type) {}

OfdLock::OfdLock(OfdLock&& other): fd(other.fd), type(other.type) {
  other.fd = -1;
}

OfdLock& OfdLock::operator=(OfdLock&& other) {
  if (this != &other) {
    // Release existing lock, if any.
    if (fd >= 0) {
      auto fl = makeFlock(F_UNLCK);
      fcntl(fd, F_OFD_SETLK, &fl);
    }
    fd = other.fd;
    type = other.type;
    other.fd = -1;
  }
  return *this;
}

OfdLock::~OfdLock() {
  if (fd >= 0) {
    auto fl = makeFlock(F_UNLCK);
    fcntl(fd, F_OFD_SETLK, &fl);
  }
}

#else  // !__linux__

kj::Maybe<OfdLock> OfdLock::tryLock(int fd, Type type) {
  KJ_FAIL_REQUIRE("OFD locks are only supported on Linux. "
                  "Cluster mode is currently implemented only on Linux.");
}

void OfdLock::verifyHeld() {
  KJ_FAIL_REQUIRE("OFD locks are only supported on Linux. "
                  "Cluster mode is currently implemented only on Linux.");
}

OfdLock::OfdLock(int fd, Type type): fd(fd), type(type) {}

OfdLock::OfdLock(OfdLock&& other): fd(other.fd), type(other.type) {
  other.fd = -1;
}

OfdLock& OfdLock::operator=(OfdLock&& other) {
  if (this != &other) {
    fd = other.fd;
    type = other.type;
    other.fd = -1;
  }
  return *this;
}

OfdLock::~OfdLock() {}

#endif  // __linux__

}  // namespace workerd
