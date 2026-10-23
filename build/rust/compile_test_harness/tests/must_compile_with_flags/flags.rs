// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The suite passes `--cfg=fixture_cfg` and edition 2021, where `gen` is not yet a keyword.

#[cfg(not(fixture_cfg))]
compile_error!("rustc_flags did not reach rustc");

pub fn f() -> u32 {
    let gen = 1;
    gen
}
