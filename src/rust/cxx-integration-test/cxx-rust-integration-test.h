#pragma once

#include <kj/function.h>

#if (defined(__has_feature) && __has_feature(thread_sanitizer)) || defined(__SANITIZE_THREAD__)
#include <sanitizer/tsan_interface.h>
#endif

#define operatorCALL operator()

namespace workerd::rust::test {

using TestCallback = kj::Function<size_t(size_t, size_t)>;

}  // namespace workerd::rust::test
