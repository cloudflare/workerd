#include "zstd-router.h"

#include <stddef.h>

#include <atomic>

// This translation unit defines the unprefixed spellings of every ZSTD_* function consumed in
// the build and forwards each call to one of the two implementations linked into the binary:
//
//   - the C implementation, rebuilt with its routed entry points renamed to zstd_c_* by
//     @zstd//:zstd_impl, and
//   - the Rust decoder (ruzstd), exported under zstd_rs_* by src/rust/zstd-rs.
//
// The Rust side implements decompression only (no credible pure-Rust encoder exists), so just
// the decompression context entry points branch on the gate; compression always goes to the C
// implementation. The error helpers are pure functions over the standard error code encoding,
// which the Rust decoder also produces, so they too always go to the C implementation. It cannot
// include zstd.h: the consumer headers declare the unprefixed spellings defined here, so the
// prototypes for both implementations are hand-declared with opaque pointers. This target
// deliberately has no kj/capnp dependencies.

extern "C" {

// Always forwarded to the C implementation.
#define ZSTD_ROUTER_C_API(V)                                                                       \
  V(void*, ZSTD_createCCtx, (void), ())                                                            \
  V(size_t, ZSTD_freeCCtx, (void* cctx), (cctx))                                                   \
  V(size_t, ZSTD_CCtx_reset, (void* cctx, int reset), (cctx, reset))                               \
  V(size_t, ZSTD_CCtx_setParameter, (void* cctx, int param, int value), (cctx, param, value))      \
  V(size_t, ZSTD_CCtx_setPledgedSrcSize, (void* cctx, unsigned long long pledgedSrcSize),          \
      (cctx, pledgedSrcSize))                                                                      \
  V(size_t, ZSTD_compressStream2, (void* cctx, void* output, void* input, int endOp),              \
      (cctx, output, input, endOp))                                                                \
  V(unsigned, ZSTD_isError, (size_t code), (code))                                                 \
  V(int, ZSTD_getErrorCode, (size_t code), (code))                                                 \
  V(const char*, ZSTD_getErrorName, (size_t code), (code))                                         \
  V(const char*, ZSTD_getErrorString, (int code), (code))

// Branches on the gate.
#define ZSTD_ROUTER_D_API(V)                                                                       \
  V(void*, ZSTD_createDCtx, (void), ())                                                            \
  V(size_t, ZSTD_freeDCtx, (void* dctx), (dctx))                                                   \
  V(size_t, ZSTD_DCtx_reset, (void* dctx, int reset), (dctx, reset))                               \
  V(size_t, ZSTD_DCtx_setParameter, (void* dctx, int param, int value), (dctx, param, value))      \
  V(size_t, ZSTD_decompressStream, (void* dctx, void* output, void* input), (dctx, output, input))

#define V(ret, name, params, args) ret zstd_c_##name params;
ZSTD_ROUTER_C_API(V)
ZSTD_ROUTER_D_API(V)
#undef V
#define V(ret, name, params, args) ret zstd_rs_##name params;
ZSTD_ROUTER_D_API(V)
#undef V

}  // extern "C"

namespace {
std::atomic<bool> useZstdRs{false};
}  // namespace

extern "C" {

void workerd_zstd_router_set_rs(bool enable) {
  useZstdRs.store(enable, std::memory_order_relaxed);
}

#define V(ret, name, params, args)                                                                 \
  ret name params {                                                                                \
    return zstd_c_##name args;                                                                     \
  }
ZSTD_ROUTER_C_API(V)
#undef V

#define V(ret, name, params, args)                                                                 \
  ret name params {                                                                                \
    return useZstdRs.load(std::memory_order_relaxed) ? zstd_rs_##name args : zstd_c_##name args;   \
  }
ZSTD_ROUTER_D_API(V)
#undef V

}  // extern "C"
