// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "test-bridge.h"

#include "test-bridge-state.h"

namespace workerd::frankenvalue::test {

kj::Maybe<Frankenvalue> consumed;

void consumeByValue(Frankenvalue value) {
  consumed = kj::mv(value);
}

void consume_frankenvalue(kj::Own<Frankenvalue> value) {
  consumeByValue(kj::mv(*value));
}

}  // namespace workerd::frankenvalue::test
