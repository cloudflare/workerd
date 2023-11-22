#pragma once

#include <kj/async-io.h>

namespace workerd {

kj::Own<kj::AsyncIoStream> newNullIoStream();
kj::Own<kj::AsyncInputStream> newNullInputStream();
kj::Own<kj::AsyncOutputStream> newNullOutputStream();

kj::Own<kj::AsyncInputStream> newMemoryInputStream(kj::ArrayPtr<const kj::byte>);
kj::Own<kj::AsyncInputStream> newMemoryInputStream(kj::StringPtr);

// An InputStream that can be disconnected.
class NeuterableInputStream: public kj::AsyncInputStream,
                             public kj::Refcounted {
public:
  virtual void neuter(kj::Exception ex) = 0;
};

class NeuterableIoStream: public kj::AsyncIoStream,
                          public kj::Refcounted {
public:
  virtual void neuter(kj::Exception ex) = 0;
};

kj::Own<NeuterableInputStream> newNeuterableInputStream(kj::AsyncInputStream&);
kj::Own<NeuterableIoStream> newNeuterableIoStream(kj::AsyncIoStream&);


}  // namespace workerd
