// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "autogate.h"
#include <workerd/util/sentry.h>
#include <kj/common.h>

namespace workerd::util {

kj::Maybe<Autogate> globalAutogate;

kj::StringPtr KJ_STRINGIFY(AutogateKey key) {
  switch (key) {
    case AutogateKey::TEST_WORKERD:
      return "test-workerd"_kj;
    case AutogateKey::LOCAL_DEV_PYTHON_SNAPSHOT:
      return "local-dev-python-snapshot"_kj;
    case AutogateKey::NumOfKeys:
      KJ_FAIL_ASSERT("NumOfKeys should not be used in getName");
  }
}

Autogate::Autogate(capnp::List<capnp::Text>::Reader autogates) {
  // Init all gates to false.
  for (AutogateKey i = AutogateKey(0); i < AutogateKey::NumOfKeys; i = AutogateKey((int)i + 1)) {
    gates[(unsigned long)i] = false;
  }

  for (auto name : autogates) {
    if (!name.startsWith("workerd-autogate-")) {
      LOG_ERROR_ONCE("Autogate configuration includes gate with invalid prefix.");
      continue;
    }
    auto sliced = name.slice(17);

    // Parse the gate name into a AutogateKey.
    for (AutogateKey i = AutogateKey(0); i < AutogateKey::NumOfKeys; i = AutogateKey((int)i + 1)) {
      if (kj::str(i) == sliced) {
        gates[(unsigned long)i] = true;
        break;
      }
    }
  }
}

bool Autogate::isEnabled(AutogateKey key) {
  KJ_IF_SOME(a, globalAutogate) {
    return a.gates[(unsigned long)key];
  }
  LOG_ERROR_PERIODICALLY(
      kj::str("Autogates not initialised, check for ", key, " will have no effect"));
  return false;
}

void Autogate::initAutogate(capnp::List<capnp::Text>::Reader gates) {
  globalAutogate = Autogate(gates);
}

void Autogate::deinitAutogate() { globalAutogate = kj::none; }

}  // namespace workerd::server
