// Minimal streaming wrapper over the zlib-rs (libz-rs-sys) C API for
// benchmarking. Lives in its own translation unit because the chromium zlib
// headers rename the standard zlib identifiers (chromeconf.h Cr_z_ mangling),
// so the unprefixed zlib-rs symbols must be declared in a TU that does not
// include <zlib.h>.

#pragma once

#include <cstddef>
#include <cstdint>

namespace workerd::zrs {

struct Stream;

Stream* newDeflate(int level, int windowBits, int memLevel, int strategy);
Stream* newInflate(int windowBits);
void freeDeflate(Stream* stream);
void freeInflate(Stream* stream);

// Runs one deflate()/inflate() call with the given buffers. Returns the zlib
// status code and reports remaining input/output space.
int runDeflate(Stream* stream,
    const uint8_t* nextIn,
    uint32_t availIn,
    uint8_t* nextOut,
    uint32_t availOut,
    int flush,
    uint32_t* availInAfter,
    uint32_t* availOutAfter);
int runInflate(Stream* stream,
    const uint8_t* nextIn,
    uint32_t availIn,
    uint8_t* nextOut,
    uint32_t availOut,
    int flush,
    uint32_t* availInAfter,
    uint32_t* availOutAfter);

}  // namespace workerd::zrs
