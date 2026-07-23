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
    case AutogateKey::ENABLE_FAST_TEXTENCODER:
      return "enable-fast-textencoder"_kj;
    case AutogateKey::UPDATED_AUTO_ALLOCATE_CHUNK_SIZE:
      return "updated-auto-allocate-chunk-size"_kj;
    case AutogateKey::STARTTLS_REJECT_EXPECTED_SERVER_HOSTNAME:
      return "starttls-reject-expected-server-hostname"_kj;
    case AutogateKey::HIBERNATABLE_WEBSOCKET_REFACTOR:
      return "hibernatable-websocket-refactor"_kj;
    case AutogateKey::PER_ISOLATE_JAVASCRIPT_BOOTSTRAP:
      return "per-isolate-javascript-bootstrap"_kj;
    case AutogateKey::DURABLE_OBJECT_RETRIES_FETCH:
      return "durable-object-retries-fetch"_kj;
    case AutogateKey::NODEJS_URL_RUST:
      return "nodejs-url-rust"_kj;
    case AutogateKey::NODEJS_EXCEPTIONS_RUST:
      return "nodejs-exceptions-rust"_kj;
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

void Autogate::initAutogate(
    capnp::List<capnp::Text>::Reader gates, IgnoreAllAutogatesEnv ignoreEnv) {
  // If the WORKERD_ALL_AUTOGATES env var is set, enable all gates regardless of what
  // was passed in. This ensures the @all-autogates test variant works even when
  // initAutogate({}) is called early (e.g. by TestFixture), which would otherwise
  // set globalAutogate to all-false and prevent isEnabled() from reaching its env var
  // fallback.
  //
  // Callers (e.g. the production server) that manage the all-autogates behavior themselves and
  // build selective gate configs can pass IgnoreAllAutogatesEnv::YES to skip this override.
  if (!ignoreEnv && getenv("WORKERD_ALL_AUTOGATES") != nullptr) {
    return initAllAutogates();
  }
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
  initAutogateNamesForTest(kj::ArrayPtr<const kj::StringPtr>(gateNames.begin(), gateNames.size()));
}

void Autogate::initAutogateNamesForTest(kj::ArrayPtr<const kj::StringPtr> gateNames) {
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
