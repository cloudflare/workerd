#pragma once

#include <v8.h>

namespace workerd::rust::jsg {
void detach_rust_wrapper(v8::Isolate* isolate, void* wrapper);
}
