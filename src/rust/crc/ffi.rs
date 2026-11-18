// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! C++ interface, used by `src/workerd/api/crypto/crc-impl.c++`.

#![allow(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]

#[cxx::bridge(namespace = "workerd::rust::crc")]
mod bridge {
    extern "Rust" {
        /// Continues the CRC-64/NVME checksum `crc` of earlier input over `data`, and returns the
        /// checksum of both; `crc` is 0 when there is no earlier input. Like zlib's `crc32()`, it
        /// takes and returns finished checksums: CRC-64/NVME's initial value and final XOR are
        /// both all ones, so the CRC register is the checksum with every bit inverted.
        fn crc64nvme(crc: u64, data: &[u8]) -> u64;
    }
}

use crate::crc64nvme;
