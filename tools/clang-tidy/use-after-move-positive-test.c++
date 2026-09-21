// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

namespace kj {
template <typename T>
T&& mv(T& value) {
  return static_cast<T&&>(value);
}

template <typename T>
struct Own {
  T& operator*() const;
};
}  // namespace kj

struct Value {
  Value clone() const;
};

void consume(Value&&);
void consumeOwn(kj::Own<Value>&&);
Value convert(Value&&);
void consumePair(Value, Value);
void use(const Value&);

void abortSignalOrdering(Value value) {
  consumePair(value.clone(), convert(kj::mv(value)));
}

void doubleMove(Value value) {
  consumePair(kj::mv(value), kj::mv(value));
}

void straightLine(Value value) {
  consume(kj::mv(value));
  use(value);
}

void dereferenceMovedOwner(kj::Own<Value> own) {
  consumeOwn(kj::mv(own));
  use(*own);
}
