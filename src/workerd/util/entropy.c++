#include "entropy.h"
#include <openssl/rand.h>

namespace workerd {

namespace {

class EntropySourceImpl final: public kj::EntropySource {
public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    KJ_ASSERT(RAND_bytes(buffer.begin(), buffer.size()) == 1);
  }
};

// TODO(cleanup): Do we actually need these variations? They are used in
// various tests but the variations may not actually be useful.
class MockEntropySource final: public kj::EntropySource {
public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    for (kj::byte& b: buffer) {
     b = counter++;
    }
  }
private:
  kj::byte counter = 0;
};

class FakeEntropySource final: public kj::EntropySource {
public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    static constexpr kj::byte DUMMY[4] = {12, 34, 56, 78};

    for (auto i: kj::indices(buffer)) {
      buffer[i] = DUMMY[i % sizeof(DUMMY)];
    }
  }
};

class FixedCharEntropySource final: public kj::EntropySource {
public:
  FixedCharEntropySource(char filler): filler(filler) {}
  void generate(kj::ArrayPtr<kj::byte> buffer) {
    memset(buffer.begin(), filler, buffer.size());
  }

private:
  char filler;
};

}  // namespace

kj::EntropySource& getEntropySource() {
  static EntropySourceImpl instance;
  return instance;
}

kj::Own<kj::EntropySource> getMockEntropySource(kj::Maybe<char> filler) {
  KJ_IF_SOME(f, filler) {
    return kj::heap<FixedCharEntropySource>(f);
  }
  return kj::heap<MockEntropySource>();
}

kj::EntropySource& getFakeEntropySource() {
  static FakeEntropySource instance;
  return instance;
}

}  // namespace workerd
