#include "future.h"

#include <kj/debug.h>

extern "C" {
void cxxbridge$kjrs$future$logError(const char* ptr, size_t len) noexcept {
  KJ_LOG(ERROR, kj::str(kj::arrayPtr(ptr, len)));
}
}
