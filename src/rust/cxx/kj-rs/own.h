#pragma once

#include <kj/memory.h>

// Drops the `kj::Own<T>` of any primitive `T` a bridge accepts: Rust calls it from `KjOwn<T>::drop`
// through `kj_rs::OwnTarget`. A primitive is never polymorphic, so `kj::Own<T>::~Own()` hands the
// disposer the pointer unchanged, exactly as `kj::Own<void>::~Own()` does, and one untyped drop
// serves them all. A bridge generates a typed drop for each `extern "C++"` type it holds in a
// `KjOwn`, where that is not true.
extern "C" {
void cxxbridge$kjrs$own$primitive$drop(kj::Own<void>* own);
}
