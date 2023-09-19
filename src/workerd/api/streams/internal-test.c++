// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "internal.h"
#include "readable.h"
#include "writable.h"
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/jsg-test.h>
#include <openssl/rand.h>

namespace workerd::api {
namespace {

template <int size>
class FooStream : public ReadableStreamSource {
public:
  FooStream() : ptr(&data[0]), remaining_(size) {
    KJ_ASSERT(RAND_bytes(data, size) == 1);
  }
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) {
    maxMaxBytesSeen_ = kj::max(maxMaxBytesSeen_, maxBytes);
    numreads_++;
    if (remaining_ == 0) return (size_t)0;
    KJ_ASSERT(minBytes == maxBytes);
    KJ_ASSERT(maxBytes <= size);
    auto amount = kj::min(remaining_, maxBytes);
    memcpy(buffer, ptr, amount);
    ptr += amount;
    remaining_ -= amount;
    return amount;
  }

  kj::ArrayPtr<uint8_t> buf() { return data; }

  size_t remaining() { return remaining_; }

  size_t numreads() { return numreads_; }

  size_t maxMaxBytesSeen() { return maxMaxBytesSeen_; }

private:
  uint8_t data[size];
  uint8_t* ptr;
  size_t remaining_;
  size_t numreads_ = 0;
  size_t maxMaxBytesSeen_ = 0;
};

template <int size>
class BarStream : public FooStream<size> {
public:
  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) {
    return size;
  }
};

KJ_TEST("test") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // In this first case, the stream does not report a length. The maximum
  // number of reads should be 3, and each allocation should be 4096
  FooStream<10000> stream;

  stream.readAllBytes(10001).then([&](auto bytes) {
    KJ_ASSERT(bytes.size() == 10000);
    KJ_ASSERT(memcmp(bytes.begin(), stream.buf().begin(), 10000) == 0);
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 3);
  KJ_ASSERT(stream.maxMaxBytesSeen() == 4096);
}

KJ_TEST("test (text)") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // In this first case, the stream does not report a length. The maximum
  // number of reads should be 3, and each allocation should be 4096
  FooStream<10000> stream;

  stream.readAllText(10001).then([&](auto bytes) {
    KJ_ASSERT(bytes.size() == 10000);
    KJ_ASSERT(memcmp(bytes.begin(), stream.buf().begin(), 10000) == 0);
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 3);
  KJ_ASSERT(stream.maxMaxBytesSeen() == 4096);
}

KJ_TEST("test2") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // In this second case, the stream does report a size, so we should see
  // only one read.
  BarStream<10000> stream;

  stream.readAllBytes(10001).then([&](auto bytes) {
    KJ_ASSERT(bytes.size() == 10000);
    KJ_ASSERT(memcmp(bytes.begin(), stream.buf().begin(), 10000) == 0);
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 2);
  KJ_ASSERT(stream.maxMaxBytesSeen() == 10000);
}

KJ_TEST("test2 (text)") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // In this second case, the stream does report a size, so we should see
  // only one read.
  BarStream<10000> stream;

  stream.readAllText(10001).then([&](auto bytes) {
    KJ_ASSERT(bytes.size() == 10000);
    KJ_ASSERT(memcmp(bytes.begin(), stream.buf().begin(), 10000) == 0);
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 2);
  KJ_ASSERT(stream.maxMaxBytesSeen() == 10000);
}

KJ_TEST("zero-length stream") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  class Zero : public ReadableStreamSource {
  public:
    kj::Promise<size_t> tryRead(void*, size_t, size_t) {
      return (size_t)0;
    }
    kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) {
      return (size_t)0;
    }
  };

  Zero zero;
  zero.readAllBytes(10).then([&](kj::Array<kj::byte> bytes) {
    KJ_ASSERT(bytes.size() == 0);
  }).wait(waitScope);
}

KJ_TEST("lying stream") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  class Dishonest : public FooStream<10000> {
  public:
    kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) {
      return (size_t)10;
    }
  };

  Dishonest stream;
  stream.readAllBytes(10001).then([&](kj::Array<kj::byte> bytes) {
    // The stream lies! it says there are only 10 bytes but there are more.
    // oh well, we at least make sure we get the right result.
    KJ_ASSERT(bytes.size() == 10000);
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 1001);
  KJ_ASSERT(stream.maxMaxBytesSeen() == 10);
}

KJ_TEST("honest small stream") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  class HonestSmall : public FooStream<100> {
  public:
    kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) {
      return (size_t)100;
    }
  };

  HonestSmall stream;
  stream.readAllBytes(1001).then([&](kj::Array<kj::byte> bytes) {
    KJ_ASSERT(bytes.size() == 100);
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 2);
  KJ_ASSERT(stream.maxMaxBytesSeen(), 100);
}

}  // namespace
}  // namespace workerd::api
