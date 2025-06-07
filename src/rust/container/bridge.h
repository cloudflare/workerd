#pragma once

#include <workerd/rust/cxx-integration/cxx-bridge.h>

#include <kj/function.h>

#define operatorCall operator();

namespace workerd::rust::container {
using MessageCallback = kj::Function<void(::rust::Slice<const uint8_t>)>;
}
