#pragma once

#include <kj/async-io.h>
#include <kj/refcount.h>

namespace workerd {

kj::Own<kj::AsyncIoStream> newNullIoStream();
kj::Own<kj::AsyncInputStream> newNullInputStream();
kj::Own<kj::AsyncOutputStream> newNullOutputStream();

// Get a shared global null output stream (singleton, thread-safe)
kj::AsyncOutputStream& getGlobalNullOutputStream();

// Returns a stream that reads `data` in place. The stream and its tee branches share `data`'s
// claim on the backing storage, so tryTee() needs no additional buffering. Create `data` from an
// owner, e.g. `kj::rc<kj::Array<kj::byte>>(kj::mv(bytes))`, or by projecting from one.
kj::Own<kj::AsyncInputStream> newMemoryInputStream(kj::Rc<kj::ArrayPtr<const kj::byte>> data);

// An InputStream that can be disconnected.
class NeuterableInputStream: public kj::AsyncInputStream, public kj::Refcounted {
 public:
  virtual void neuter(kj::Exception ex) = 0;
  virtual bool isNeutered() const = 0;
};

class NeuterableIoStream: public kj::AsyncIoStream, public kj::Refcounted {
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
kj::Rc<NeuterableIoStream> newNeuterableIoStream(kj::AsyncIoStream&);

}  // namespace workerd
