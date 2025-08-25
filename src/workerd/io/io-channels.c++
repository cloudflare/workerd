#include "io-channels.h"

namespace workerd {

void IoChannelFactory::ActorChannel::requireAllowsTransfer() {
  JSG_FAIL_REQUIRE(DOMDataCloneError,
      "Durable Object stubs cannot (yet) be transferred between Workers. This will change in "
      "a future version.");
}

uint IoChannelCapTableEntry::getChannelNumber(Type expectedType) {
  // A type mismatch shouldn't be possible as long as attackers cannot tamper with the
  // serialization, but we do the check to catch bugs.
  KJ_REQUIRE(type == expectedType,
      "IoChannelCapTableEntry type didn't match serialized JavaScript API type.");

  return channel;
}

kj::Own<Frankenvalue::CapTableEntry> IoChannelCapTableEntry::clone() {
  return kj::heap<IoChannelCapTableEntry>(type, channel);
}

kj::Own<Frankenvalue::CapTableEntry> IoChannelCapTableEntry::threadSafeClone() const {
  return kj::heap<IoChannelCapTableEntry>(type, channel);
}

}  // namespace workerd
