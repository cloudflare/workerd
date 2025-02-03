#include <workerd/rust/async/future.h>

namespace workerd::rust::async {

// =======================================================================================
// Boilerplate follows

using BoxFutureVoid = BoxFuture<void>;

// We define this pointer typedef so that cxx-rs can associate it with the same pointer type our
// drop function uses.
using PtrBoxFutureVoid = BoxFutureVoid*;

using BoxFutureFulfillerVoid = BoxFutureFulfiller<void>;

// ---------------------------------------------------------

using BoxFutureFallibleVoid = BoxFuture<Fallible<void>>;

// We define this the pointer typedef so that cxx-rs can associate it with the same pointer type our
// drop function uses.
using PtrBoxFutureFallibleVoid = BoxFutureFallibleVoid*;

using BoxFutureFulfillerFallibleVoid = BoxFutureFulfiller<Fallible<void>>;

// ---------------------------------------------------------

using BoxFutureFallibleI32 = BoxFuture<Fallible<int32_t>>;

// We define this pointer typedef so that cxx-rs can associate it with the same pointer type our
// drop function uses.
using PtrBoxFutureFallibleI32 = BoxFutureFallibleI32*;

using BoxFutureFulfillerFallibleI32 = BoxFutureFulfiller<Fallible<int32_t>>;

}  // namespace workerd::rust::async
