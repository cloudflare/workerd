// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <algorithm>
#include <cstring>
#include <list>
#include <kj/string.h>

namespace workerd {

template<size_t StackSize>
class StringBuffer {
  // String buffer optimized for appending a lot of strings together.
  // Allocates StackSize chunk on the stack and uses that until full.
  // Keeps allocating new chunks of at least HeapChunkSize as needed.
  // Doesn't perform any heap allocations if string stays within
  // StackSize bytes (without \0)

public:
  KJ_DISALLOW_COPY_AND_MOVE(StringBuffer);

  explicit StringBuffer(size_t heapChunkSize): heapChunkSize(heapChunkSize), tail(&arr[0]), cap(StackSize) {}

  void append() {}

  template <typename First, typename... Rest>
  void append(First&& first, Rest&&...rest) {
    appendImpl(kj::fwd<First>(first));
    append(kj::fwd<Rest>(rest)...);
  }

  kj::String toString() {
    auto result = kj::heapString(len);
    copyTo(result.begin());
    return result;
  }

private:
  const size_t heapChunkSize;
  // minimum heap chunk size

  char arr[StackSize];
  // chunk on the stack

  std::list<kj::Array<char>> chunks;
  // on the heap chunks

  char *tail;
  // points after the last used bytes in current chunk

  size_t cap;
  // number of bytes available in current chunk

  size_t len = 0;
  // total length of the data appended so far

  void appendImpl(const char* ptr, size_t size) {
    size_t toCopy = std::min(size, cap);
    memcpy(tail, ptr, toCopy);
    tail += toCopy;
    cap -= toCopy;

    if (toCopy != size) {
      // prepare new chunk
      size_t remaining = size - toCopy;
      size_t chunkSize = std::max(remaining, heapChunkSize); // don't chunk large strings
      auto chunk = kj::heapArray<char>(chunkSize);

      // copy the rest of the string to the new chunk
      memcpy(chunk.begin(), ptr + toCopy, remaining);
      tail = chunk.begin() + remaining;
      cap = chunk.size() - remaining;

      chunks.push_back(kj::mv(chunk));
    }

    len += size;
  }

  void appendImpl(const kj::StringPtr& str) {
    appendImpl(str.begin(), str.size());
  }

  template<size_t size>
  void appendImpl(const char (&arr)[size]) {
    appendImpl(arr, size - 1 /* assume 0-terminated strings */);
  }

  inline void appendImpl(const kj::ArrayPtr<const char>& arr) {
    appendImpl(arr.begin(), arr.size());
  }

  inline void appendImpl(const kj::String& str) {
    appendImpl(str.asPtr());
  }

  void copyTo(char* dest) {
    // copy stack portion first
    size_t onStack = std::min(len, StackSize);
    memcpy(dest, arr, onStack);
    dest += onStack;

    // copy from heap chunks
    if (onStack < len) {
      size_t remaining = len - onStack;
      for (auto& chunk: chunks) {
        size_t inChunk = std::min(remaining, chunk.size()); // last chunk won't be full
        memcpy(dest, chunk.begin(), inChunk);
        dest += inChunk;
        remaining -= inChunk;
      }

      KJ_IREQUIRE(remaining == 0);
    }
  }
};

} // namespace workerd
