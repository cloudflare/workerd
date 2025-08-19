#include "io-channels.h"

namespace workerd {

void IoChannelFactory::ActorChannel::requireAllowsTransfer() {
  JSG_FAIL_REQUIRE(DOMDataCloneError,
      "Durable Object stubs cannot (yet) be transferred between Workers. This will change in "
      "a future version.");
}

}  // namespace workerd
