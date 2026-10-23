#pragma once

#include <workerd/io/observer.h>
#include <workerd/io/worker-source.h>

namespace workerd {

// Counts bundle source bodies before parsing. Service Worker globals contribute bytes but do
// not count as modules; Cap'n Proto schemas have no module source bodies to count.
IsolateObserver::ScriptSourceStats computeScriptSourceStats(const WorkerSource& source);

}  // namespace workerd
