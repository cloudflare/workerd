// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/string.h>
#include <capnp/blob.h>
#include <capnp/list.h>
#include <initializer_list>

namespace workerd::util {

// Workerd-specific list of autogate keys (can also be used in internal repo).
enum class AutogateKey {
  TEST_WORKERD,
  PYODIDE_LOAD_EXTERNAL,
  // Enables reporting of disconnection during deferred proxying as a new status.
  RESPONSE_STREAM_DISCONNECTED_STATUS,
  NumOfKeys // Reserved for iteration.
};

// This class allows code changes to be rolled out independent of full binary releases. It enables
// specific code paths to be gradually rolled out via our internal tooling.
// See the equivalent file in our internal repo for more details.
//
// Workerd-specific gates can be added here.
//
// Usage:
//
//     #include <workerd/util/autogate.h>
//     Autogate::isEnabled(AutogateKey::YOUR_FEATURE_KEY)
//
// When making structural changes here, ensure you align them with autogate.h in the internal repo.
class Autogate {

public:
  static bool isEnabled(AutogateKey key);

  // Creates a global Autogate and seeds it with gates that are specified in the config.
  //
  // This function is not thread safe, it should be called exactly once close to the start of the
  // process before any threads are created.
  static void initAutogate(
      capnp::List<capnp::Text>::Reader autogates);

  // Convenience method for bin-tests to invoke initAutogate() with an appropriate config.
  static void initAutogateNamesForTest(std::initializer_list<kj::StringPtr> gateNames);

  // Destroys an initialised global Autogate instance. Used only for testing.
  static void deinitAutogate();
private:
  bool gates[(unsigned long)AutogateKey::NumOfKeys];

  Autogate(capnp::List<capnp::Text>::Reader autogates);
};

// Retrieves the name of the gate.
//
// When adding a new gate, add it into this method as well.
kj::StringPtr KJ_STRINGIFY(AutogateKey key);

}  // namespace workerd::util
