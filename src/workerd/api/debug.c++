#include "debug.h"

#include <workerd/util/autogate.h>

namespace workerd::api {

bool InternalDebugModule::autogateIsEnabled(jsg::Lock&, kj::String name) {
  const char* prefix = "workerd-autogate-";

  for (auto i: kj::zeroTo(static_cast<size_t>(util::AutogateKey::NumOfKeys))) {
    auto key = static_cast<util::AutogateKey>(i);
    if (util::Autogate::isEnabled(key) && name == kj::str(prefix, key)) {
      return true;
    }
  }
  return false;
}
}  // namespace workerd::api
