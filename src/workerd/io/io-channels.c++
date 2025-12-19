#include "io-channels.h"

namespace workerd {

kj::Array<byte> IoChannelFactory::SubrequestChannel::getToken(ChannelTokenUsage usage) {
  JSG_FAIL_REQUIRE(DOMDataCloneError, "This ServiceStub cannot be serialized.");
}

kj::Array<byte> IoChannelFactory::ActorClassChannel::getToken(ChannelTokenUsage usage) {
  JSG_FAIL_REQUIRE(DOMDataCloneError, "This Durable Object class cannot be serialized.");
}

kj::Own<IoChannelFactory::SubrequestChannel> IoChannelFactory::subrequestChannelFromToken(
    ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  JSG_FAIL_REQUIRE(DOMDataCloneError, "This Worker is not able to deserialize ServiceStubs.");
}

kj::Own<IoChannelFactory::ActorClassChannel> IoChannelFactory::actorClassFromToken(
    ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  JSG_FAIL_REQUIRE(
      DOMDataCloneError, "This Worker is not able to deserialize Durable Object class stubs.");
}

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
