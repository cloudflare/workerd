#pragma once

#include <kj/common.h>

namespace workerd::util {

kj::Maybe<size_t> tryGetResidentSetMemory();

}  // namespace workerd::util
