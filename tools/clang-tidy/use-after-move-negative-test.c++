// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

namespace kj {
template <typename T>
T&& mv(T& value) {
  return static_cast<T&&>(value);
}

}  // namespace kj

namespace capnp {
template <typename T>
struct RemotePromise {};
}  // namespace capnp

struct Value {};

void consume(Value&&);
void consumeRemote(capnp::RemotePromise<Value>&&);
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

#define KJ_IF_SOME(body)                                                                           \
  do {                                                                                             \
    body                                                                                           \
  } while (false)

void kjControlFlow(Value value) {
  KJ_IF_SOME(consume(kj::mv(value)); use(value);)
    ;
}

void splitMovedFromState(capnp::RemotePromise<Value> promise) {
  consumeRemote(kj::mv(promise));
  (void)promise;
}
