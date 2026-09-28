#include "own.h"

static_assert(sizeof(kj::Own<void>) == 2 * sizeof(void*), "unexpected kj::Own layout");

extern "C" {
void cxxbridge$kjrs$own$primitive$drop(kj::Own<void>* own) {
  own->~Own();
}
}
