// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include "kj/debug.h"
#include <kj/map.h>
#include <workerd/server/workerd.capnp.h>

namespace workerd::server {

// Workerd-specific list of autogate keys (can also be used in internal repo).
enum class AutogateKey {
  TEST_WORKERD,
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
//     #include <workerd/server/autogate.h>
//     Autogate::isEnabled(AutogateKey::YOUR_FEATURE_KEY)
//
// When making structural changes here, ensure you align them with autogate.h in workerd.
class Autogate {

public:
  Autogate(capnp::List<config::Config::Autogate, capnp::Kind::STRUCT>::Reader autogates);

  bool isEnabled(AutogateKey key) const;

private:
  kj::HashMap<AutogateKey, bool> gates;
};

// Retrieves the name of the gate.
//
// When adding a new gate, add it into this method as well.
kj::StringPtr KJ_STRINGIFY(AutogateKey key);

extern kj::Maybe<Autogate> globalAutogate;

void initAutogate(config::Config::Reader config);
void initAutogate(capnp::List<config::Config::Autogate, capnp::Kind::STRUCT>::Reader autogates);

}  // namespace workerd::server
