#include <atomic>

#include <kj/async.h>
#include <kj/common.h>

#include "workerd.h"
#include <capnp/compat/json.h>
#include <workerd/api/basics.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/io/worker-interface.h>
#include <workerd/jsg/jsg.h>
#include <workerd/server/workerd.capnp.h>

namespace workerd::api {

kj::Promise<kj::String> Workerd::runWorker(kj::String configJson) {
  capnp::MallocMessageBuilder confArena;
  capnp::JsonCodec json;
  json.handleByAnnotation<server::config::Config>();
  auto conf = confArena.initRoot<server::config::Config>();
  json.decode(configJson, conf);
  return host.runWorker(conf);
}

}  // namespace workerd::api
