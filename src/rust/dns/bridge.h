#pragma once

#include "v8-isolate.h"
#include "v8-value.h"

namespace workerd::rust::dns {
    using Isolate = v8::Isolate;
    using JsValue = v8::Value;
}