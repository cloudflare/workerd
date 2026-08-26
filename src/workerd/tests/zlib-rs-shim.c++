#include "zlib-rs-shim.h"

#include <kj/debug.h>

namespace workerd::zrs {
namespace {

// Standard zlib z_stream ABI, as implemented by libz-rs-sys.
extern "C" {

struct ZStream {
  const uint8_t* next_in;
  unsigned int avail_in;
  unsigned long total_in;
  uint8_t* next_out;
  unsigned int avail_out;
  unsigned long total_out;
  char* msg;
  void* state;
  void* (*zalloc)(void*, unsigned int, unsigned int);
  void (*zfree)(void*, void*);
  void* opaque;
  int data_type;
  unsigned long adler;
  unsigned long reserved;
};

// zlib-rs C API.
extern const char* zlibVersion(void);
extern int deflateInit2_(ZStream* strm,
    int level,
    int method,
    int windowBits,
    int memLevel,
    int strategy,
    const char* version,
    int stream_size);
extern int inflateInit2_(ZStream* strm, int windowBits, const char* version, int stream_size);
extern int deflate(ZStream* strm, int flush);
extern int inflate(ZStream* strm, int flush);
extern int deflateEnd(ZStream* strm);
extern int inflateEnd(ZStream* strm);

}  // extern "C"

constexpr int Z_DEFLATED_METHOD = 8;

}  // namespace

Stream* newDeflate(int level, int windowBits, int memLevel, int strategy) {
  auto stream = new ZStream{};
  int result = deflateInit2_(stream, level, Z_DEFLATED_METHOD, windowBits, memLevel, strategy,
      zlibVersion(), sizeof(ZStream));
  KJ_REQUIRE(result == 0, "zlib-rs deflateInit2 failed", result);
  return reinterpret_cast<Stream*>(stream);
}

Stream* newInflate(int windowBits) {
  auto stream = new ZStream{};
  int result = inflateInit2_(stream, windowBits, zlibVersion(), sizeof(ZStream));
  KJ_REQUIRE(result == 0, "zlib-rs inflateInit2 failed", result);
  return reinterpret_cast<Stream*>(stream);
}

void freeDeflate(Stream* stream) {
  auto zs = reinterpret_cast<ZStream*>(stream);
  deflateEnd(zs);
  delete zs;
}

void freeInflate(Stream* stream) {
  auto zs = reinterpret_cast<ZStream*>(stream);
  inflateEnd(zs);
  delete zs;
}

int runDeflate(Stream* stream,
    const uint8_t* nextIn,
    uint32_t availIn,
    uint8_t* nextOut,
    uint32_t availOut,
    int flush,
    uint32_t* availInAfter,
    uint32_t* availOutAfter) {
  auto zs = reinterpret_cast<ZStream*>(stream);
  zs->next_in = nextIn;
  zs->avail_in = availIn;
  zs->next_out = nextOut;
  zs->avail_out = availOut;
  int result = deflate(zs, flush);
  *availInAfter = zs->avail_in;
  *availOutAfter = zs->avail_out;
  return result;
}

int runInflate(Stream* stream,
    const uint8_t* nextIn,
    uint32_t availIn,
    uint8_t* nextOut,
    uint32_t availOut,
    int flush,
    uint32_t* availInAfter,
    uint32_t* availOutAfter) {
  auto zs = reinterpret_cast<ZStream*>(stream);
  zs->next_in = nextIn;
  zs->avail_in = availIn;
  zs->next_out = nextOut;
  zs->avail_out = availOut;
  int result = inflate(zs, flush);
  *availInAfter = zs->avail_in;
  *availOutAfter = zs->avail_out;
  return result;
}

}  // namespace workerd::zrs
