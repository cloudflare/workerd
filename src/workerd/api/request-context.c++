#include "request-context.h"
#include <workerd/jsg/async-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

kj::Maybe<jsg::Value> RequestContextModule::getRequestId(jsg::Lock& js) {
  if (IoContext::hasCurrent()) {
    KJ_IF_MAYBE(frame, jsg::AsyncContextFrame::current(js)) {
      return frame->get(IoContext::current().getRequestIdKey()).map([&](jsg::Value& val) {
        return val.addRef(js);
      });
    }
  }
  return nullptr;
}

}  // namespace workerd::api
