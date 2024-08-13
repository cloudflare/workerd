#pragma once

// based on https://github.com/rsms/dawn-wire-example/blob/main/pipe.hh

#include <algorithm>
#include <cstring>
#include <kj/async-io.h>
#include <kj/debug.h>
#include <limits>
#include <vector>
#ifdef _WIN32
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/types.h>
#endif


namespace workerd::api::gpu::voodoo {

// Pipe is a circular read-write buffer.
// It works like this:
//
// initial:       storage: 0 1 2 3 4 5 6 7
// len: 0                  |
//                        w r
//
// write 5 bytes: storage: 0 1 2 3 4 5 6 7
// len: 5                  |         |
//                         r         w
//
// read 2 bytes:  storage: 0 1 2 3 4 5 6 7
// len: 3                      |     |
//                             r     w
//
// write 4 bytes: storage: 0 1 2 3 4 5 6 7
// len: 7                    | |
//                           w r
//
template <size_t Size> struct Pipe {
  // the len function assumes Size < MAX_SIZE_T/2
  static_assert(Size < std::numeric_limits<size_t>::max() / 2, "Size < MAX_SIZE_T/2");

  char _storage[Size];
  size_t _w = 0; // storage write offset
  size_t _r = 0; // storage read offset

  constexpr size_t cap() const {
    return Size - 1;
  }
  size_t len() const {
    return (Size - _r + _w) % Size;
  }
  size_t avail() const {
    return (Size - 1 - _w + _r) % Size;
  }

  // add data to the beginning of the pipe
  size_t write(const char* src, size_t nbyte); // copy <=nbyte of dst into the pipe
  size_t writec(char c);                       // add c to the pipe
  kj::Promise<ssize_t> readFromStream(kj::Own<kj::AsyncIoStream>& stream,
                                      size_t nbyte); // read <=nbyte from stream (-1 on error)

  // take data out of the end of the pipe
  size_t read(char* dst, size_t nbyte); // copy <=nbyte of data to dst
  size_t discard(size_t nbyte);         // read & discard
  kj::Promise<ssize_t> writeToStream(kj::Own<kj::AsyncIoStream>& stream,
                                     size_t nbyte); // write <=nbyte to file (-1 on error)

  // takeRef removes nbyte and returns a pointer to the removed bytes,
  // if and only if the next nbytes are contiguous, i.e. does not span across the
  // underlying ring buffer's head & tail. Returns nullptr on failure.
  // The returned memory is only valid until the next call to write() or clear().
  const char* takeRef(size_t nbyte);

  inline char at(size_t index) const {
    return _storage[_r + index];
  }

  // clear drains the pipe by discarding any data waiting to be read
  void clear() {
    _w = 0;
    _r = 0;
  }
};

template <size_t Size> size_t Pipe<Size>::write(const char* data, size_t nbyte) {
  nbyte = std::min(nbyte, avail());
  size_t chunkend = std::min(nbyte, Size - _w);
  memcpy(_storage + _w, data, chunkend);
  memcpy(_storage, data + chunkend, nbyte - chunkend);
  _w = (_w + nbyte) % Size;
  return nbyte;
}

template <size_t Size> size_t Pipe<Size>::writec(char c) {
  if (avail() == 0) {
    return 0;
  }
  _storage[_w] = c;
  _w = (_w + 1) % Size;
  return 1;
}

size_t writec(char c);

template <size_t Size>
kj::Promise<ssize_t> Pipe<Size>::readFromStream(kj::Own<kj::AsyncIoStream>& stream, size_t nbyte) {
  nbyte = std::min(nbyte, avail());
  size_t chunkend = std::min(nbyte, Size - _w);
  ssize_t total = 0;
  if (chunkend > 0) {
    KJ_LOG(INFO, "will read", _w, chunkend);
    total = co_await stream->read(_storage + _w, 0, chunkend);
    KJ_LOG(INFO, "read", total);
    if (total < (ssize_t)chunkend) {
      // short read
      if (total > -1) {
        goto end;
      }
      co_return total;
    }
  }
  if (nbyte > chunkend) {
    ssize_t n = co_await stream->read(_storage, 0, nbyte - chunkend);
    if (n < 0) {
      co_return n;
    }
    total += n;
  }
end:
  _w = (_w + (size_t)total) % Size;
  co_return total;
}

template <size_t Size>
kj::Promise<ssize_t> Pipe<Size>::writeToStream(kj::Own<kj::AsyncIoStream>& stream, size_t nbyte) {
  nbyte = std::min(nbyte, len());
  size_t chunkend = std::min(nbyte, Size - _r);
  ssize_t total = 0;
  const kj::byte* ptr = reinterpret_cast<const kj::byte*>(_storage);
  if (chunkend > 0) {
    KJ_LOG(INFO, "will write", _storage, _r, chunkend, stream);
    co_await stream->write(kj::arrayPtr<const kj::byte>(ptr + _r, chunkend));
    total = chunkend;
    KJ_LOG(INFO, "wrote", total);
  }
  if (nbyte > chunkend) {
    auto left = nbyte - chunkend;
    co_await stream->write(kj::arrayPtr<const kj::byte>(ptr, left));
    total += left;
  }

  _r = (_r + nbyte) % Size;
  co_return total;
}

template <size_t Size> size_t Pipe<Size>::read(char* data, size_t nbyte) {
  nbyte = std::min(nbyte, len());
  size_t chunkend = std::min(nbyte, Size - _r);
  memcpy(data, _storage + _r, chunkend);
  memcpy(data + chunkend, _storage, nbyte - chunkend);
  _r = (_r + nbyte) % Size;
  return nbyte;
}

template <size_t Size> size_t Pipe<Size>::discard(size_t nbyte) {
  nbyte = std::min(nbyte, len());
  _r = (_r + nbyte) % Size;
  return nbyte;
}

template <size_t Size> const char* Pipe<Size>::takeRef(size_t nbyte) {
  // Either w is ahead of e in memory ...
  //   0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
  //      W2   |        R1        |    W1      R=read-from, W=write-to
  //           r                  w
  // ... or r is ahead of w in memory ...
  //   0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
  //      R2   |        W1        |    R1
  //           w                  r
  // In either case we can only return a reference to R1.
  nbyte = std::min(nbyte, len());
  size_t chunkend = std::min(nbyte, Size - _r);
  const char* p = nullptr;
  if (chunkend >= nbyte) {
    p = _storage + _r;
    _r = (_r + nbyte) % Size;
  }
  return p;
}

} // namespace workerd::api::gpu::voodoo
