#pragma once
#include <workerd/io/worker-interface.h>
#include <workerd/io/limit-enforcer.h>
#include <workerd/server/workerd.capnp.h>

namespace workerd {

// IsolateLimitEnforcer that enforces no limits.
kj::Own<workerd::IsolateLimitEnforcer> newNullIsolateLimitEnforcer();

kj::Own<workerd::IsolateLimitEnforcer> newConfiguredIsolateLimitEnforcer(
  server::config::Worker::Limits::Reader configuredLimits);

}  // namespace workerd
