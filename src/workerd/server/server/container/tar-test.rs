// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn field(header: &[u8], range: std::ops::Range<usize>) -> &str {
    let bytes = &header[range];
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    std::str::from_utf8(&bytes[..end]).unwrap()
}

#[test]
fn one_file() {
    let tar = tar_with_file("hello.txt", b"hello world").unwrap();
    // Header, one block of content, two end blocks.
    assert_eq!(tar.len(), 4 * 512);
    let header = &tar[..512];
    assert_eq!(field(header, 0..100), "hello.txt");
    assert_eq!(field(header, 100..108), "0000644");
    assert_eq!(field(header, 124..136), "00000000013");
    assert_eq!(header[156], b'0');
    assert_eq!(field(header, 257..263), "ustar");
    assert_eq!(&tar[512..523], b"hello world");
    assert!(tar[523..].iter().all(|&b| b == 0));

    // The checksum is the sum of the header's bytes with the checksum field read as spaces.
    let mut copy = header.to_vec();
    copy[148..156].fill(b' ');
    let sum: u32 = copy.iter().map(|&b| u32::from(b)).sum();
    assert_eq!(field(header, 148..155), format!("{sum:06o} "));
    assert_eq!(header[155], b' ');
}

#[test]
fn content_padded_to_blocks() {
    assert_eq!(tar_with_file("a", &[]).unwrap().len(), 3 * 512);
    assert_eq!(tar_with_file("a", &[1; 512]).unwrap().len(), 4 * 512);
    assert_eq!(tar_with_file("a", &[1; 513]).unwrap().len(), 5 * 512);
}

#[test]
fn long_name_refused() {
    assert!(tar_with_file(&"a".repeat(99), b"").is_ok());
    let error = tar_with_file(&"a".repeat(100), b"").unwrap_err();
    assert!(
        error
            .description()
            .contains("tar filename must be < 100 bytes")
    );
}
