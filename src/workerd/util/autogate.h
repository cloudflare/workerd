// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <capnp/blob.h>
#include <capnp/list.h>
#include <kj/string.h>

#include <initializer_list>

namespace workerd::util {

// Workerd-specific list of autogate keys (can also be used in internal repo).
enum class AutogateKey {
  TEST_WORKERD,
  RESCHEDULE_DESYNCED_SQLITE_ALARMS,
  V8_FAST_API,
  NumOfKeys  // Reserved for iteration.
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
  static void initAutogate(capnp::List<capnp::Text>::Reader autogates);

  // Convenience method for bin-tests to invoke initAutogate() with an appropriate config.
  static void initAutogateNamesForTest(std::initializer_list<kj::StringPtr> gateNames);

  // Initializes all autogates, enabling all features.
  //
  // This is used by the --all-autogates flag in the CLI to enable all gates regardless
  // of what's specified in the config file. This can be useful during development to test
  // features that are behind autogates.
  //
  // CAUTION: Be aware that enabling all gates may expose you to experimental features
  // that aren't ready for production use. Use with care in production environments.
  static void initAllAutogates();

  // Destroys an initialized global Autogate instance. Used only for testing.
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
