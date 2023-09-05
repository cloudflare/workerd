// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/vector.h>

namespace workerd {

template<typename T, size_t InlineSize>
class SmallVector {
public:
  KJ_DISALLOW_COPY(SmallVector);
  SmallVector() {}
  SmallVector(SmallVector&&) = default;

  inline void add(T&& t) {
    if (len < InlineSize) {
      arr[len] = kj::mv(t);
      len++;
    } else {
      if (len == InlineSize) {
        vec = kj::Vector<T>(len + 1);
        auto& v = KJ_ASSERT_NONNULL(vec);
        for (size_t i = 0; i < len; i++) {
          v.add(kj::mv(arr[i]));
        }
      }
      KJ_ASSERT_NONNULL(vec).add(kj::mv(t));
      len++;
    }
  }

  inline T* begin() { KJ_FAIL_REQUIRE("NOT IMPLEMENTED"); }
  inline T* end() { KJ_FAIL_REQUIRE("NOT IMPLEMENTED"); }
  inline const T* begin() const { KJ_FAIL_REQUIRE("NOT IMPLEMENTED"); }
  inline const T* end() const { KJ_FAIL_REQUIRE("NOT IMPLEMENTED"); }

  kj::ArrayPtr<T> asPtr() { KJ_FAIL_REQUIRE("NOT IMPLEMENTED"); }

  void clear() { KJ_FAIL_REQUIRE("NOT IMPLEMENTED"); }
  size_t size() const { return len; }

  inline T& operator[](size_t index) { KJ_FAIL_REQUIRE("NOT IMPLEMENTED"); }
  inline const T& operator[](size_t index) const { KJ_FAIL_REQUIRE("NOT IMPLEMENTED"); }

private:
  size_t len = 0;
  T arr[InlineSize];
  kj::Maybe<kj::Vector<T>> vec;
};


} // namespace workerd
