// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

namespace kj {
template <typename T>
T&& mv(T& value) {
  return static_cast<T&&>(value);
}

}  // namespace kj

struct Value {};

void consume(Value&&);
void use(const Value&);
bool choose();

void mutuallyExclusive(Value value) {
  if (choose()) {
    consume(kj::mv(value));
  } else {
    use(value);
  }
}

void reinitialized(Value value) {
  consume(kj::mv(value));
  value = Value();
  use(value);
}

void loopLocal() {
  while (choose()) {
    Value value;
    consume(kj::mv(value));
  }
}

void sequenced(Value value) {
  consume((use(value), kj::mv(value)));
}

void unevaluated(Value value) {
  (void)sizeof(kj::mv(value));
  use(value);
}

struct Base {
  Base();
  Base(Base&& other);
};

struct Derived: Base {
  Derived(Derived&& other)
      : Base(kj::mv(other)),
        first(kj::mv(other.first)),
        second(kj::mv(other.second)) {}

  Value first;
  Value second;
};

#define KJ_CASE_ONEOF(name, value)                                                                 \
  for (auto &name = value, *name##Done = &name; name##Done; name##Done = nullptr)

void kjControlFlow(Value value) {
  KJ_CASE_ONEOF(selected, value) {
    consume(kj::mv(selected));
  }
}
