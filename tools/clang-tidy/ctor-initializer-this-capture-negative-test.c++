// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Negative fixtures for workerd-ctor-initializer-this-capture: none of these
// may produce a diagnostic.

namespace fixtures {

class Callback {
 public:
  Callback() = default;
  template <typename F>
  Callback(F&& f) {}  // NOLINT(google-explicit-constructor)
};

template <typename F>
int compute(F&& f) {
  return 0;
}

class Widget {
 public:
  // Case N1: the lambda is created in the constructor body, after every
  // member has been initialized.
  explicit Widget(int) {
    onEvent = [this]() { handle(); };
  }

  // Case N2: lambdas that capture nothing, or only constructor parameters.
  Widget(int a, int& b): onEvent([a, &b]() { b += a; }), other([] {}), value(a) {}

  // Case N3: an immediately invoked lambda does not outlive the initializer.
  explicit Widget(long v)
      : onEvent([] {}),
        other([] {}),
        value([&]() { return v > 0 ? first : static_cast<int>(v); }()) {}

  // Case N4: same, with an explicit `this` capture.
  explicit Widget(short v): onEvent([] {}), other([] {}), value([this]() { return first; }()) {}

  // Case N5: a lambda passed to a function that captures only parameters.
  explicit Widget(char v): onEvent([] {}), other([] {}), value(compute([v]() { return v; })) {}

  // Case N6: explicit suppression.
  explicit Widget(bool)
      : onEvent([this]() { handle(); }),  // NOLINT(workerd-ctor-initializer-this-capture)
        other([] {}),
        value(0) {}

  void handle() {
    // Case N7: capturing `this` in an ordinary member function.
    onEvent = [this]() { handle(); };
  }

 private:
  int first = 0;
  // Case N8: default member initializers are not checked.
  Callback onEvent = [this]() { handle(); };
  Callback other;
  int value;
};

// Case N9: a lambda in a constructor body of a class nested in another
// constructor's initializer is not part of that initializer's object.
class Outer {
 public:
  Outer()
      : callback([]() {
          struct Local {
            Local() {
              cb = [this]() {};
            }
            Callback cb;
          };
          Local local;
        }) {}

 private:
  Callback callback;
};

}  // namespace fixtures
