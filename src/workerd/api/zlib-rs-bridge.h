#pragma once

// Bridge to zlib-rs (libz-rs-sys), the memory-safe Rust implementation of the
// zlib C API. The functions take the z_stream as void* because the bridge
// implementation must be compiled in a translation unit that does not include
// workerd's <zlib.h>: the chromium zlib fork renames all zlib identifiers
// (chromeconf.h Cr_z_ mangling), while zlib-rs exports the standard names.
// The z_stream ABI is identical between the two implementations.

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
