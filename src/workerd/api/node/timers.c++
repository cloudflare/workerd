#include "timers.h"

#include <workerd/api/global-scope.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

// The setImmediate/clearImmediate methods are only exposed on globalThis if the
// node_compat_v2 flag is set. However, we want them exposed via `node:timers`
// generally when just the original node_compat is enabled. Therefore, we provide
// this alternative route to the implementations on ServiceWorkerGlobalScope.
jsg::Ref<Immediate> TimersUtil::setImmediate(jsg::Lock& js,
    jsg::Function<void(jsg::Arguments<jsg::Value>)> function,
    jsg::Arguments<jsg::Value> args) {
  auto context = js.v8Context();
  auto& global =
      jsg::extractInternalPointer<ServiceWorkerGlobalScope, true>(context, context->Global());
  return global.setImmediate(js, kj::mv(function), kj::mv(args));
}

void TimersUtil::clearImmediate(jsg::Lock& js, kj::Maybe<jsg::Ref<Immediate>> maybeImmediate) {
  auto context = js.v8Context();
  auto& global =
      jsg::extractInternalPointer<ServiceWorkerGlobalScope, true>(context, context->Global());
  global.clearImmediate(kj::mv(maybeImmediate));
}
}  // namespace workerd::api::node
