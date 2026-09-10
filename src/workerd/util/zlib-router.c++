#include "zlib-router.h"

#include <atomic>

// This translation unit defines the standard unprefixed zlib C API and forwards each call to one
// of the two implementations linked into the binary:
//
//   - chromium zlib, whose symbols are mangled to Cr_z_* by chromeconf.h, and
//   - zlib-rs (libz-rs-sys), exported under zlib_rs_* by src/rust/zlib-rs.
//
// It cannot include zlib.h: a single TU sees either the unprefixed or the Cr_z_ spelling of the
// declarations, never both, so both implementations' prototypes are hand-declared here. The
// z_stream ABI (and the version/stream_size init handshake) is identical between the two, so
// streams pass through as opaque pointers. This target deliberately has no kj/capnp dependencies
// so that anything linking @zlib (e.g. V8 tooling) stays lean.

extern "C" {

#define ZLIB_ROUTER_API(V)                                                                         \
  V(const char*, zlibVersion, (void), ())                                                          \
  V(int, deflateInit_, (void* strm, int level, const char* version, int stream_size),              \
      (strm, level, version, stream_size))                                                         \
  V(int, deflateInit2_,                                                                            \
      (void* strm, int level, int method, int windowBits, int memLevel, int strategy,              \
          const char* version, int stream_size),                                                   \
      (strm, level, method, windowBits, memLevel, strategy, version, stream_size))                 \
  V(int, inflateInit_, (void* strm, const char* version, int stream_size),                         \
      (strm, version, stream_size))                                                                \
  V(int, inflateInit2_, (void* strm, int windowBits, const char* version, int stream_size),        \
      (strm, windowBits, version, stream_size))                                                    \
  V(int, deflate, (void* strm, int flush), (strm, flush))                                          \
  V(int, inflate, (void* strm, int flush), (strm, flush))                                          \
  V(int, deflateEnd, (void* strm), (strm))                                                         \
  V(int, inflateEnd, (void* strm), (strm))                                                         \
  V(int, deflateReset, (void* strm), (strm))                                                       \
  V(int, inflateReset, (void* strm), (strm))                                                       \
  V(int, inflateReset2, (void* strm, int windowBits), (strm, windowBits))                          \
  V(int, deflateParams, (void* strm, int level, int strategy), (strm, level, strategy))            \
  V(int, deflateSetDictionary,                                                                     \
      (void* strm, const unsigned char* dictionary, unsigned int dictLength),                      \
      (strm, dictionary, dictLength))                                                              \
  V(int, inflateSetDictionary,                                                                     \
      (void* strm, const unsigned char* dictionary, unsigned int dictLength),                      \
      (strm, dictionary, dictLength))                                                              \
  V(unsigned long, deflateBound, (void* strm, unsigned long sourceLen), (strm, sourceLen))         \
  V(int, deflateSetHeader, (void* strm, void* head), (strm, head))                                 \
  V(unsigned long, crc32, (unsigned long crc, const unsigned char* buf, unsigned int len),         \
      (crc, buf, len))                                                                             \
  V(unsigned long, adler32, (unsigned long adler, const unsigned char* buf, unsigned int len),     \
      (adler, buf, len))                                                                           \
  V(int, compress,                                                                                 \
      (unsigned char* dest, unsigned long* destLen, const unsigned char* source,                   \
          unsigned long sourceLen),                                                                \
      (dest, destLen, source, sourceLen))                                                          \
  V(int, compress2,                                                                                \
      (unsigned char* dest, unsigned long* destLen, const unsigned char* source,                   \
          unsigned long sourceLen, int level),                                                     \
      (dest, destLen, source, sourceLen, level))                                                   \
  V(unsigned long, compressBound, (unsigned long sourceLen), (sourceLen))                          \
  V(int, uncompress,                                                                               \
      (unsigned char* dest, unsigned long* destLen, const unsigned char* source,                   \
          unsigned long sourceLen),                                                                \
      (dest, destLen, source, sourceLen))

// Prototypes for both implementations.
#define V(ret, name, params, args)                                                                 \
  ret Cr_z_##name params;                                                                          \
  ret zlib_rs_##name params;
ZLIB_ROUTER_API(V)
#undef V

}  // extern "C"

namespace {
std::atomic<bool> useZlibRs{false};
}  // namespace

extern "C" {

void workerd_zlib_router_set_rs(bool enable) {
  useZlibRs.store(enable, std::memory_order_relaxed);
}

#define V(ret, name, params, args)                                                                 \
  ret name params {                                                                                \
    return useZlibRs.load(std::memory_order_relaxed) ? zlib_rs_##name args : Cr_z_##name args;     \
  }
ZLIB_ROUTER_API(V)
#undef V

}  // extern "C"
