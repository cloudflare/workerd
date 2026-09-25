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

namespace kj {
template <typename T>
struct Own {
  T* get() const;
  bool operator==(decltype(nullptr)) const;
};

template <typename T>
struct Rc {
  T* get() const;
};

template <typename T>
struct Arc {
  T* get() const;
};
}  // namespace kj

void consumeOwn(kj::Own<Value>&&);
void consumeRc(kj::Rc<Value>&&);
void consumeArc(kj::Arc<Value>&&);
bool check(bool);

// Like std::unique_ptr, KJ's owning pointers are null after a move, so inspecting one without
// dereferencing it is well-defined.
void inspectMovedKjPointers(kj::Own<Value> own, kj::Rc<Value> rc, kj::Arc<Value> arc) {
  consumeOwn(kj::mv(own));
  check(own.get() == nullptr);
  check(own == nullptr);
  consumeRc(kj::mv(rc));
  check(rc.get() == nullptr);
  consumeArc(kj::mv(arc));
  check(arc.get() == nullptr);
}
