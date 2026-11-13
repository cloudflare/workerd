#include "messagechannel.h"

#include <workerd/api/per-isolate-bootstrap.h>

namespace workerd::api {

namespace {

jsg::JsValue getBootstrapClass(jsg::Lock& js, kj::StringPtr name) {
  // main.ts requires the module in every context.
  auto exports = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, "messagechannel/messagechannel"_kj),
      "the per-isolate bootstrap did not load messagechannel/messagechannel");
  return KJ_ASSERT_NONNULL(exports.tryCast<jsg::JsObject>()).get(js, name);
}

}  // namespace

jsg::JsValue MessageChannelModule::getMessageChannel(jsg::Lock& js) {
  return getBootstrapClass(js, "MessageChannel"_kj);
}

jsg::JsValue MessageChannelModule::getMessagePort(jsg::Lock& js) {
  return getBootstrapClass(js, "MessagePort"_kj);
}

}  // namespace workerd::api
