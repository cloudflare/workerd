#pragma once

#include <kj/array.h>
#include <kj/common.h>
#include <kj/string.h>

namespace workerd {

class SslArrayDisposer: public kj::ArrayDisposer {
public:
  static SslArrayDisposer INSTANCE;
private:
  void disposeImpl(void* firstElement, size_t elementSize, size_t elementCount,
                   size_t capacity, void (*destroyElement)(void*)) const override;
};

struct PemData {
  kj::String type;
  kj::Array<kj::byte> data;
};

kj::Maybe<PemData> decodePem(kj::ArrayPtr<const char> text);

}
