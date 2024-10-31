#pragma once

#include <kj/function.h>

#define operatorCALL operator()

namespace workerd::rust::test {

using TestCallback = kj::Function<size_t(size_t, size_t)>;

using UsizeCallback = kj::Function<void(size_t)>;

}  // namespace edgeworker::rust::test
