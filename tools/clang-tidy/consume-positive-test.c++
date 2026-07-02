// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

namespace kj {

template <typename T>
T&& mv(T& value) {
  return static_cast<T&&>(value);
}

template <typename T>
class Ptr {
 public:
  explicit Ptr(T* ptr): ptr(ptr) {}

  T* operator->() {
    return ptr;
  }

 private:
  T* ptr;
};

}  // namespace kj

#ifdef __clang__
#define WD_CONSUME __attribute__((annotate("workerd_consume")))
#else
#define WD_CONSUME
#endif

struct Base {
  virtual void mustConsume() WD_CONSUME {}
};

struct Derived final: public Base {
  void mustConsume() override {}
};

void positive(kj::Ptr<Derived> direct) {
  direct->mustConsume();
}
