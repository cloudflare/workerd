#pragma once

#include <kj/function.h>

#define operatorCALL operator()

namespace workerd::rust::test {

using TestCallback = kj::Function<size_t(size_t, size_t)>;

}  // namespace workerd::rust::test
