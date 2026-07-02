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

namespace workerd {

template <typename T>
class [[nodiscard]] ConsumePtr final {
 public:
  explicit ConsumePtr(kj::Ptr<T>&& ptr): ptr(kj::mv(ptr)) {}

  T* operator->() && {
    T* result = ptr.operator->();
    ptr = kj::Ptr<T>(nullptr);
    return result;
  }

 private:
  kj::Ptr<T> ptr;
};

template <typename T>
[[nodiscard]] ConsumePtr<T> consume(kj::Ptr<T>&& ptr) {
  return ConsumePtr<T>(kj::mv(ptr));
}

}  // namespace workerd

#ifdef __clang__
#define WD_CONSUME __attribute__((annotate("workerd_consume")))
#else
#define WD_CONSUME
#endif

struct Base {
  virtual void mustConsume() WD_CONSUME {}
  virtual void ordinary() {}
};

struct Derived final: public Base {
  void mustConsume() override {}
  void ordinary() override {}
};

void negative(kj::Ptr<Derived> safe, kj::Ptr<Derived> ordinary) {
  workerd::consume(kj::mv(safe))->mustConsume();
  ordinary->ordinary();
}
