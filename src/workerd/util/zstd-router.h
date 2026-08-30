#pragma once

// Routing layer owning the unprefixed spellings of every ZSTD_* symbol consumed in the build.
// zstd consumers resolve to these definitions (see zstd-router.c++), which forward each call to
// either the C implementation (zstd_c_*) or the Rust decoder (zstd_rs_*).

extern "C" {

// Selects the Rust decoder as the backing implementation for all subsequent zstd decompression
// calls; compression stays on the C implementation on both sides. Set at process startup from
// the compression-rs autogate (see Autogate::initAutogate()); a decompression context must be
// created, run, and freed by a single implementation, so this must not be toggled while any
// context is live.
void workerd_zstd_router_set_rs(bool enable);

}  // extern "C"
