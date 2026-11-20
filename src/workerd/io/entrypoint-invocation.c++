// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "entrypoint-invocation.h"

#include <workerd/api/global-scope.h>

namespace workerd::entrypoint {

HandlerHandles getHandlerHandles(Worker::Lock& lock,
    IoContext& context,
    kj::Maybe<kj::StringPtr> entrypointName,
    kj::Maybe<Worker::VersionInfo> versionInfo,
    Frankenvalue props) {
  jsg::Lock& js = lock;
  HandlerHandles handles{
    .handler = js.v8Undefined(),
    .env = js.v8Undefined(),
    .ctx = js.v8Undefined(),
  };
  KJ_IF_SOME(h,
      lock.getExportedHandler(
          entrypointName, kj::mv(versionInfo), kj::mv(props), context.getActor())) {
    // The handles stay valid in the lock's HandleScope once the ExportedHandler is gone.
    handles.handler = h->self.getHandle(js);
    handles.env = h->env.getHandle(js);
    KJ_IF_SOME(c, h->getCtx()) {
      handles.ctx = lock.getWorker().getIsolate().getApi().wrapExecutionContext(js, kj::mv(c));
    }
  }
  return handles;
}

}  // namespace workerd::entrypoint
