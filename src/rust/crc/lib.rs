// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! CRCs computed by the `crc-fast` crate, for `crypto.DigestStream`: CRC-64/NVME.

pub mod ffi;

use crc_fast::CrcAlgorithm;
use crc_fast::Digest;

/// Continues the CRC-64/NVME `crc` of earlier input over `data`; `crc` is 0 for no earlier input.
#[must_use]
pub fn crc64nvme(crc: u64, data: &[u8]) -> u64 {
    // CRC-64/NVME's initial value and final XOR are both all ones, so the checksum is the CRC
    // register with every bit inverted. Inverting it again (`!` is Rust's bitwise NOT, C++'s `~`)
    // recovers the register; for `crc` 0 that is the initial value. `finalize()` inverts the
    // register back into a checksum (it XORs it with `xorout`, all ones for CRC-64/NVME).
    let mut digest = Digest::new_with_init_state(CrcAlgorithm::Crc64Nvme, !crc);
    digest.update(data);
    digest.finalize()
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
