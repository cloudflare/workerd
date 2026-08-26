#include "zlib-rs-bridge.h"

// The zlib-rs C ABI, declared at global scope.
extern "C" {
const char* zlibVersion(void);
int deflateInit2_(void* strm,
    int level,
    int method,
    int windowBits,
    int memLevel,
    int strategy,
    const char* version,
    int stream_size);
int inflateInit2_(void* strm, int windowBits, const char* version, int stream_size);
int deflate(void* strm, int flush);
int inflate(void* strm, int flush);
int deflateEnd(void* strm);
int inflateEnd(void* strm);
int deflateReset(void* strm);
int inflateReset(void* strm);
int deflateParams(void* strm, int level, int strategy);
int deflateSetDictionary(void* strm, const uint8_t* dictionary, uint32_t dictLength);
int inflateSetDictionary(void* strm, const uint8_t* dictionary, uint32_t dictLength);
}

namespace workerd::api::zlibrs {
namespace {

// The standard zlib z_stream layout, mirrored here (with native C types, so
// the size is correct on both LP64 and LLP64) purely to compute the
// stream_size consistency-check argument that the init functions validate.
struct ZStreamLayout {
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
constexpr int Z_STREAM_SIZE = sizeof(ZStreamLayout);
constexpr int Z_DEFLATED_METHOD = 8;

}  // namespace

int initDeflate(void* strm, int level, int windowBits, int memLevel, int strategy) {
  return ::deflateInit2_(strm, level, Z_DEFLATED_METHOD, windowBits, memLevel, strategy,
      ::zlibVersion(), Z_STREAM_SIZE);
}

int initInflate(void* strm, int windowBits) {
  return ::inflateInit2_(strm, windowBits, ::zlibVersion(), Z_STREAM_SIZE);
}

int runDeflate(void* strm, int flush) {
  return ::deflate(strm, flush);
}

int runInflate(void* strm, int flush) {
  return ::inflate(strm, flush);
}

int endDeflate(void* strm) {
  return ::deflateEnd(strm);
}

int endInflate(void* strm) {
  return ::inflateEnd(strm);
}

int resetDeflate(void* strm) {
  return ::deflateReset(strm);
}

int resetInflate(void* strm) {
  return ::inflateReset(strm);
}

int setDeflateParams(void* strm, int level, int strategy) {
  return ::deflateParams(strm, level, strategy);
}

int setDeflateDictionary(void* strm, const uint8_t* dictionary, uint32_t dictLength) {
  return ::deflateSetDictionary(strm, dictionary, dictLength);
}

int setInflateDictionary(void* strm, const uint8_t* dictionary, uint32_t dictLength) {
  return ::inflateSetDictionary(strm, dictionary, dictLength);
}

}  // namespace workerd::api::zlibrs
