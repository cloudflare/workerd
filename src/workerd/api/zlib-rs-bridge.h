#pragma once

// Bridge to zlib-rs (libz-rs-sys), the memory-safe Rust implementation of the
// zlib C API. The functions take z_stream as void* to keep zlib types out of
// this interface.

#include <cstdint>

namespace workerd::api::zlibrs {

int initDeflate(void* strm, int level, int windowBits, int memLevel, int strategy);
int initInflate(void* strm, int windowBits);
int runDeflate(void* strm, int flush);
int runInflate(void* strm, int flush);
int endDeflate(void* strm);
int endInflate(void* strm);
int resetDeflate(void* strm);
int resetInflate(void* strm);
int setDeflateParams(void* strm, int level, int strategy);
int setDeflateDictionary(void* strm, const uint8_t* dictionary, uint32_t dictLength);
int setInflateDictionary(void* strm, const uint8_t* dictionary, uint32_t dictLength);

}  // namespace workerd::api::zlibrs
