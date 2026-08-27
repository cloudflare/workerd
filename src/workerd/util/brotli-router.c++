#include "brotli-router.h"

#include <stddef.h>

#include <atomic>

// This translation unit defines the brotli C API entry points our consumers use and forwards
// each call to one of the two implementations linked into the binary:
//
//   - the C implementation, compiled with these functions renamed to brotli_c_* (see
//     build/BUILD.brotli), and
//   - rust-brotli, exported under brotli_rs_* by src/rust/brotli-rs.
//
// It does not include the brotli headers: the renames in build/BUILD.brotli only apply to the
// implementation's own compilation, so the prototypes are hand-declared here with the encoder and
// decoder states passed through as opaque pointers. Enum parameters and results travel as int.
// This target deliberately has no kj/capnp dependencies so that anything linking @brotli stays
// lean.

extern "C" {

typedef void* (*brotli_alloc_func)(void* opaque, size_t size);
typedef void (*brotli_free_func)(void* opaque, void* address);

#define BROTLI_ROUTER_API(V)                                                                       \
  V(void*, BrotliEncoderCreateInstance,                                                            \
      (brotli_alloc_func alloc_func, brotli_free_func free_func, void* opaque),                    \
      (alloc_func, free_func, opaque))                                                             \
  V(void, BrotliEncoderDestroyInstance, (void* state), (state))                                    \
  V(int, BrotliEncoderSetParameter, (void* state, int param, unsigned int value),                  \
      (state, param, value))                                                                       \
  V(int, BrotliEncoderCompressStream,                                                              \
      (void* state, int op, size_t* availableIn, const unsigned char** nextIn,                     \
          size_t* availableOut, unsigned char** nextOut, size_t* totalOut),                        \
      (state, op, availableIn, nextIn, availableOut, nextOut, totalOut))                           \
  V(int, BrotliEncoderIsFinished, (void* state), (state))                                          \
  V(int, BrotliEncoderHasMoreOutput, (void* state), (state))                                       \
  V(size_t, BrotliEncoderMaxCompressedSize, (size_t inputSize), (inputSize))                       \
  V(int, BrotliEncoderCompress,                                                                    \
      (int quality, int lgwin, int mode, size_t inputSize, const unsigned char* inputBuffer,       \
          size_t* encodedSize, unsigned char* encodedBuffer),                                      \
      (quality, lgwin, mode, inputSize, inputBuffer, encodedSize, encodedBuffer))                  \
  V(void*, BrotliDecoderCreateInstance,                                                            \
      (brotli_alloc_func alloc_func, brotli_free_func free_func, void* opaque),                    \
      (alloc_func, free_func, opaque))                                                             \
  V(void, BrotliDecoderDestroyInstance, (void* state), (state))                                    \
  V(int, BrotliDecoderSetParameter, (void* state, int param, unsigned int value),                  \
      (state, param, value))                                                                       \
  V(int, BrotliDecoderDecompressStream,                                                            \
      (void* state, size_t* availableIn, const unsigned char** nextIn, size_t* availableOut,       \
          unsigned char** nextOut, size_t* totalOut),                                              \
      (state, availableIn, nextIn, availableOut, nextOut, totalOut))                               \
  V(int, BrotliDecoderHasMoreOutput, (const void* state), (state))                                 \
  V(int, BrotliDecoderGetErrorCode, (const void* state), (state))                                  \
  V(const char*, BrotliDecoderErrorString, (int code), (code))                                     \
  V(int, BrotliDecoderDecompress,                                                                  \
      (size_t encodedSize, const unsigned char* encodedBuffer, size_t* decodedSize,                \
          unsigned char* decodedBuffer),                                                           \
      (encodedSize, encodedBuffer, decodedSize, decodedBuffer))

// Prototypes for both implementations.
#define V(ret, name, params, args)                                                                 \
  ret brotli_c_##name params;                                                                      \
  ret brotli_rs_##name params;
BROTLI_ROUTER_API(V)
#undef V

}  // extern "C"

namespace {
std::atomic<bool> useBrotliRs{false};
}  // namespace

extern "C" {

void workerd_brotli_router_set_rs(bool enable) {
  useBrotliRs.store(enable, std::memory_order_relaxed);
}

#define V(ret, name, params, args)                                                                 \
  ret name params {                                                                                \
    return useBrotliRs.load(std::memory_order_relaxed) ? brotli_rs_##name args                     \
                                                       : brotli_c_##name args;                     \
  }
BROTLI_ROUTER_API(V)
#undef V

}  // extern "C"
