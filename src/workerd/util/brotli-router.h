#pragma once

// Routing layer owning the standard unprefixed brotli symbol names
// (BrotliEncoderCompressStream, BrotliDecoderDecompressStream, ...). Every brotli consumer in the
// build resolves the declarations in brotli/encode.h and brotli/decode.h to these definitions
// (see brotli-router.c++), which forward each call to either the C implementation (brotli_c_*)
// or rust-brotli (brotli_rs_*).

extern "C" {

// Selects rust-brotli as the backing implementation for all subsequent brotli calls. Set at
// process startup from the compression-rs autogate (see Autogate::initAutogate()); an encoder or
// decoder state must be created, run, and destroyed by a single implementation, so this must not
// be toggled while any state is live.
void workerd_brotli_router_set_rs(bool enable);

}  // extern "C"
