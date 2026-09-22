#pragma once

// Routing layer owning the standard unprefixed zlib symbol names (deflate, inflate, crc32, ...).
// Every zlib consumer in the build is compiled with CHROMIUM_ZLIB_NO_CHROMECONF and so resolves
// to these definitions (see zlib-router.c++), which forward each call to either the chromium
// implementation (Cr_z_*) or zlib-rs (zlib_rs_*).

extern "C" {

// Selects zlib-rs as the backing implementation for all subsequent zlib calls. Set at process
// startup from the compression-rs autogate (see Autogate::initAutogate()); a z_stream must be
// initialized, run, and freed by a single implementation, so this must not be toggled while any
// stream is live.
void workerd_zlib_router_set_rs(bool enable);

}  // extern "C"
