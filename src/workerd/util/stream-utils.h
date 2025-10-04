#pragma once

#include <kj/async-io.h>

namespace workerd {

kj::Own<kj::AsyncIoStream> newNullIoStream();
kj::Own<kj::AsyncInputStream> newNullInputStream();
kj::Own<kj::AsyncOutputStream> newNullOutputStream();

// Get a shared global null output stream (singleton, thread-safe)
kj::AsyncOutputStream& getGlobalNullOutputStream();

kj::Own<kj::AsyncInputStream> newMemoryInputStream(kj::ArrayPtr<const kj::byte>);
kj::Own<kj::AsyncInputStream> newMemoryInputStream(kj::StringPtr);

// An InputStream that can be disconnected.
class NeuterableInputStream: public kj::AsyncInputStream, public kj::Refcounted {
 public:
  virtual void neuter(kj::Exception ex) = 0;
};

class NeuterableIoStream: public kj::AsyncIoStream {
 public:
  virtual void neuter(kj::Exception ex) = 0;
};

// Until kj::AsyncOutputStream has an end() method of its own... We
// provide this subclass that adds it.
class EndableAsyncOutputStream: public kj::AsyncOutputStream {
 public:
  // By default, end() is a no-op. Subclasses may override.
  virtual kj::Promise<void> end() {
    co_return;
  }
};

kj::Own<NeuterableInputStream> newNeuterableInputStream(kj::AsyncInputStream&);
kj::Own<NeuterableIoStream> newNeuterableIoStream(kj::AsyncIoStream&);

}  // namespace workerd
