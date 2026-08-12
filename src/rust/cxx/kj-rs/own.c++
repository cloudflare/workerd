#include "own.h"

extern "C" {
// As of right now, this function works and passes all tests, destroying all tested objects correctly.
// This works because disposers work by virtual call, and the disposer is created during creation of
// the Own, so the type of the Own at destruction doesn't actually matter for Owns that do not use a
// static disposer, which are not currently supported by workerd-cxx.
void cxxbridge$kjrs$own$drop(void* own) {
  reinterpret_cast<kj::Own<void>*>(own)->~Own();
}
}
