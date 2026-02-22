// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "autogate.h"

#include <workerd/util/sentry.h>

#include <stdlib.h>

#include <capnp/message.h>
#include <kj/common.h>
#include <kj/debug.h>

namespace workerd::util {

kj::Maybe<Autogate> globalAutogate;

kj::StringPtr KJ_STRINGIFY(AutogateKey key) {
  switch (key) {
    case AutogateKey::TEST_WORKERD:
      return "test-workerd"_kj;
    case AutogateKey::V8_FAST_API:
      return "v8-fast-api"_kj;
    case AutogateKey::STREAMING_TAIL_WORKER:
      return "streaming-tail-worker"_kj;
    case AutogateKey::TAIL_STREAM_REFACTOR:
      return "tail-stream-refactor"_kj;
    case AutogateKey::INCREASE_EXTERNAL_MEMORY_ADJUSTMENT_FOR_FETCH:
      return "increase-external-memory-adjustment-for-fetch"_kj;
    case AutogateKey::RUST_BACKED_NODE_DNS:
      return "rust-backed-node-dns"_kj;
    case AutogateKey::RPC_USE_EXTERNAL_PUSHER:
      return "rpc-use-external-pusher"_kj;
    case AutogateKey::BLOB_USE_STREAMS_NEW_MEMORY_SOURCE:
      return "blob-use-streams-new-memory-source"_kj;
    case AutogateKey::NumOfKeys:
      KJ_FAIL_ASSERT("NumOfKeys should not be used in getName");
  }
}

Autogate::Autogate(capnp::List<capnp::Text>::Reader autogates) {
  // gates array is zero-initialized by default.
  for (auto name: autogates) {
    if (!name.startsWith("workerd-autogate-")) {
      LOG_ERROR_ONCE("Autogate configuration includes gate with invalid prefix.");
      continue;
    }
    auto sliced = name.slice(17);

    // Parse the gate name into a AutogateKey.
    for (AutogateKey i = AutogateKey(0); i < AutogateKey::NumOfKeys;
         i = AutogateKey(static_cast<int>(i) + 1)) {
      if (kj::str(i) == sliced) {
        gates[static_cast<unsigned long>(i)] = true;
        break;
      }
    }
  }
}

bool Autogate::isEnabled(AutogateKey key) {
  KJ_IF_SOME(a, globalAutogate) {
    return a.gates[static_cast<unsigned long>(key)];
  }

  static const bool defaultResult = getenv("WORKERD_ALL_AUTOGATES") != nullptr;
  return defaultResult;
}

void Autogate::initAutogate(capnp::List<capnp::Text>::Reader gates) {
  globalAutogate = Autogate(gates);
}

void Autogate::deinitAutogate() {
  globalAutogate = kj::none;
}

void Autogate::initAllAutogates() {
  Autogate autogate;
  for (AutogateKey i = AutogateKey(0); i < AutogateKey::NumOfKeys;
       i = AutogateKey(static_cast<int>(i) + 1)) {
    autogate.gates[static_cast<unsigned long>(i)] = true;
  }
  globalAutogate = kj::mv(autogate);
}

void Autogate::initAutogateNamesForTest(std::initializer_list<kj::StringPtr> gateNames) {
  capnp::MallocMessageBuilder message;
  auto orphanage = message.getOrphanage();
  auto gatesOrphan = orphanage.newOrphan<capnp::List<capnp::Text>>(gateNames.size());
  auto gates = gatesOrphan.get();
  size_t count = 0;
  for (auto name: gateNames) {
    gates.set(count++, kj::str("workerd-autogate-", name));
  }
  Autogate::initAutogate(gates.asReader());
}

}  // namespace workerd::util
