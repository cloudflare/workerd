// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A ustar archive holding one file: the Docker API takes files for a container only as tar.

use crate::Result;

/// The largest file a ustar header's 11-digit octal size field can describe.
const MAX_CONTENT_SIZE: usize = 8 * 1024 * 1024 * 1024;

const BLOCK: usize = 512;

/// Copies as much of `value` as fits into `field`.
fn write_field(field: &mut [u8], value: &[u8]) {
    let len = value.len().min(field.len());
    field[..len].copy_from_slice(&value[..len]);
}

/// A tar archive of the regular file `filename` (mode 0644, owned by root), followed by the two
/// zero blocks that end an archive.
///
/// # Errors
///
/// A name of 100 bytes or more, or content too large for the header's size field.
pub fn tar_with_file(filename: &str, content: &[u8]) -> Result<Vec<u8>> {
    if filename.len() >= 100 {
        kj::fail_require!("tar filename must be < 100 bytes");
    }
    if content.len() >= MAX_CONTENT_SIZE {
        kj::fail_require!("tar content too large for 11-digit octal");
    }

    let padded = content.len().div_ceil(BLOCK) * BLOCK;
    let mut tar = vec![0u8; BLOCK + padded + 2 * BLOCK];
    let header = &mut tar[..BLOCK];
    write_field(&mut header[..100], filename.as_bytes());
    write_field(&mut header[100..108], b"0000644");
    write_field(&mut header[108..116], b"0000000");
    write_field(&mut header[116..124], b"0000000");
    write_field(
        &mut header[124..136],
        format!("{:011o}", content.len()).as_bytes(),
    );
    write_field(&mut header[136..148], b"00000000000");
    header[156] = b'0';
    write_field(&mut header[257..263], b"ustar");
    write_field(&mut header[263..265], b"00");

    // The checksum is computed with its own field filled with spaces.
    header[148..156].fill(b' ');
    let checksum: u32 = header.iter().map(|&b| u32::from(b)).sum();
    write_field(&mut header[148..155], format!("{checksum:06o} ").as_bytes());

    tar[BLOCK..BLOCK + content.len()].copy_from_slice(content);
    Ok(tar)
}

#[cfg(test)]
#[path = "tar-test.rs"]
mod tests;
