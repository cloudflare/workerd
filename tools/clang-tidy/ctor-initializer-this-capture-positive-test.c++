// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Positive fixtures for workerd-ctor-initializer-this-capture: every case must
// produce exactly one diagnostic.

namespace fixtures {

// Type-erased stand-in for kj::Function: stores a callable.
class Callback {
 public:
  template <typename F>
  Callback(F&& f) {}  // NOLINT(google-explicit-constructor)
  Callback(const Callback&) = default;
};

class Promise {
 public:
  template <typename F>
  Promise then(F&& f) {
    return *this;
  }
};

Promise start();

template <typename F>
Callback wrap(F&& f) {
  return Callback(static_cast<F&&>(f));
}

class Base {
 public:
  explicit Base(Callback callback): callback(callback) {}

 private:
  Callback callback;
};

class Widget: public Base {
 public:
  // Case P1: explicit `this` capture.
  explicit Widget(int): Base([] {}), onEvent([this]() { handle(); }), task(start()), value(0) {}

  // Case P2: `*this` copies the partially constructed object.
  explicit Widget(long)
      : Base([] {}),
        onEvent([*this]() mutable { handle(); }),
        task(start()),
        value(0) {}

  // Case P3: implicit capture through `[&]`.
  explicit Widget(short): Base([] {}), onEvent([&]() { handle(); }), task(start()), value(0) {}

  // Case P4: implicit capture through `[=]`.
  explicit Widget(char): Base([] {}), onEvent([=]() { handle(); }), task(start()), value(0) {}

  // Case P5: base-class initializer.
  explicit Widget(float): Base([this]() { handle(); }), onEvent([] {}), task(start()), value(0) {}

  // Case P6: delegating initializer.
  explicit Widget(double): Widget(wrap([this]() { handle(); })) {}

  // Case P7: init-capture of `this`.
  explicit Widget(unsigned)
      : Base([] {}),
        onEvent([self = this]() { self->handle(); }),
        task(start()),
        value(0) {}

  // Case P8: init-capture of a reference to `*this`.
  explicit Widget(unsigned long)
      : Base([] {}),
        onEvent([&self = *this]() { self.handle(); }),
        task(start()),
        value(0) {}

  // Case P9: lambda passed to a continuation inside a larger expression.
  explicit Widget(unsigned short)
      : Base([] {}),
        onEvent([] {}),
        task(start().then([this]() { handle(); })),
        value(0) {}

  // Case P10: lambda that escapes from an immediately invoked lambda.
  explicit Widget(unsigned char)
      : Base([] {}),
        onEvent([&]() { return wrap([this]() { handle(); }); }()),
        task(start()),
        value(0) {}

  // Case P11: lambda created by an init-capture of a lambda that does not
  // itself capture `this`.
  explicit Widget(bool)
      : Base([] {}),
        onEvent([inner = wrap([this]() { handle(); })]() {}),
        task(start()),
        value(0) {}

  // Case P12: explicit `this` alongside a capture default.
  Widget(int, int): Base([] {}), onEvent([&, this]() { handle(); }), task(start()), value(0) {}

  void handle() {}

 private:
  explicit Widget(Callback callback): Base(callback), onEvent(callback), task(start()), value(0) {}

  Callback onEvent;
  Promise task;
  int value;
};

// Case P13: constructor defined out of line.
class OutOfLine {
 public:
  OutOfLine();
  void handle() {}

 private:
  Callback onEvent;
};

OutOfLine::OutOfLine(): onEvent([this]() { handle(); }) {}

// Case P14: class template, reported once despite two instantiations.
template <typename T>
class Holder {
 public:
  Holder(): onEvent([this]() { handle(); }) {}
  void handle() {}

 private:
  Callback onEvent;
};

// Case P15: class template whose capture default captures `this` only through
// a dependent member access.
template <typename T>
class DependentHolder {
 public:
  DependentHolder(): onEvent([&]() { value.handle(); }) {}

 private:
  T value;
  Callback onEvent;
};

struct Handler {
  void handle() {}
};

void instantiate() {
  Holder<int> a;
  Holder<long> b;
  DependentHolder<Handler> c;
}

}  // namespace fixtures
