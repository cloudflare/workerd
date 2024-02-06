#pragma once

#include <kj/compat/http.h>

namespace workerd {

// Returns a singleton instance of the production entropy source.
kj::EntropySource& getEntropySource();

// Used for testing purposes only.
kj::Own<kj::EntropySource> getMockEntropySource(
    kj::Maybe<char> filler = kj::none);

// Used for testing purposes only.
kj::EntropySource& getFakeEntropySource();

}  // namespace workerd
