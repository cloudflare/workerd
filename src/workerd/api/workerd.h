#pragma once

#include <atomic>

#include <kj/async.h>
#include <kj/common.h>

#include <workerd/api/basics.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/io/worker-interface.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

using kj::uint;

class HostInterface {

public:
  virtual ~HostInterface() noexcept(false) {};
  virtual kj::StringPtr version() = 0;

};

// Binding types

class Workerd: public jsg::Object {
  // A capability to Workerd itself.

public:
  Workerd(HostInterface& host)
    : host(host) {}

  kj::StringPtr getVersion() { return host.version(); }

  JSG_RESOURCE_TYPE(Workerd) {
    JSG_READONLY_INSTANCE_PROPERTY(version, getVersion);
    /* JSG_METHOD(send); */
    /* JSG_METHOD(sendBatch); */

    /* JSG_TS_ROOT(); */
    /* JSG_TS_OVERRIDE(Queue<Body> { */
    /*   send(message: Body): Promise<void>; */
    /*   sendBatch(messages: Iterable<MessageSendRequest<Body>>): Promise<void>; */
    /* }); */
  }

private:
  HostInterface& host;
};

#define EW_WORKERD_ISOLATE_TYPES \
  api::Workerd

}  // namespace workerd::api
