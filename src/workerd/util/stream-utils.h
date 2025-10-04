#pragma once

#include <kj/async-io.h>

namespace workerd {

kj::Own<kj::AsyncIoStream> newNullIoStream();
kj::Own<kj::AsyncInputStream> newNullInputStream();
kj::Own<kj::AsyncOutputStream> newNullOutputStream();

// Get a shared global null output stream (singleton, thread-safe)
kj::AsyncOutputStream& getGlobalNullOutputStream();

// When maybeBacking is provided, it is held onto by the MemoryInputStream
// using a kj::Rc<...> so that teeing the stream can share ownership of
// the backing storage without the need for any additional buffering. If
// the backing storage is not provided, then optimized teeing of the stream
// will not be supported and the implementation will return a kj::none from
// tryTee().
kj::Own<kj::AsyncInputStream> newMemoryInputStream(
    kj::ArrayPtr<const kj::byte>, kj::Maybe<kj::Own<void>> maybeBacking = kj::none);
kj::Own<kj::AsyncInputStream> newMemoryInputStream(
    kj::StringPtr, kj::Maybe<kj::Own<void>> maybeBacking = kj::none);

// An InputStream that can be disconnected.
class NeuterableInputStream: public kj::AsyncInputStream, public kj::Refcounted {
 public:
  virtual void neuter(kj::Exception ex) = 0;
};

class NeuterableIoStream: public kj::AsyncIoStream {
 public:
  virtual void neuter(kj::Exception ex) = 0;
};

kj::Own<NeuterableInputStream> newNeuterableInputStream(kj::AsyncInputStream&);
kj::Own<NeuterableIoStream> newNeuterableIoStream(kj::AsyncIoStream&);

}  // namespace workerd
