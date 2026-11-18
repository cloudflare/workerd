// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn the_tunable_is_found_only_as_a_whole_entry() {
    assert!(has_rseq_tunable("glibc.pthread.rseq=0"));
    assert!(has_rseq_tunable(
        "glibc.malloc.arena_max=2:glibc.pthread.rseq=0"
    ));
    assert!(has_rseq_tunable(
        "glibc.pthread.rseq=0:glibc.malloc.tcache_count=16"
    ));
    assert!(!has_rseq_tunable(""));
    assert!(!has_rseq_tunable("glibc.pthread.rseq=1"));
    assert!(!has_rseq_tunable("glibc.pthread.rseq=00"));
    assert!(!has_rseq_tunable("xglibc.pthread.rseq=0"));
}

#[test]
fn adding_the_tunable_keeps_the_others() {
    assert_eq!(with_rseq_tunable(""), "glibc.pthread.rseq=0");
    assert_eq!(
        with_rseq_tunable("glibc.malloc.arena_max=2"),
        "glibc.malloc.arena_max=2:glibc.pthread.rseq=0"
    );
}

#[test]
fn removing_the_tunable_keeps_the_others() {
    assert_eq!(without_rseq_tunable("glibc.pthread.rseq=0"), None);
    assert_eq!(
        without_rseq_tunable("glibc.malloc.arena_max=2:glibc.pthread.rseq=0"),
        Some("glibc.malloc.arena_max=2".to_owned())
    );
    assert_eq!(
        without_rseq_tunable("a=1:glibc.pthread.rseq=0:b=2"),
        Some("a=1:b=2".to_owned())
    );
}

#[test]
fn kernel_versions_parse_from_their_numeric_prefix() {
    assert_eq!(parse_kernel_version("7.0.12-linuxkit"), Some((7, 0, 12)));
    assert_eq!(
        parse_kernel_version("6.12.107+deb13-amd64\n"),
        Some((6, 12, 107))
    );
    assert_eq!(parse_kernel_version("7.1-rc1"), Some((7, 1, 0)));
    assert_eq!(parse_kernel_version("6"), None);
    assert_eq!(parse_kernel_version(""), None);
}

#[test]
fn only_arm64_kernels_without_the_fix_keep_per_thread_caches() {
    assert!(kernel_has_arm64_rseq_bug("aarch64", "6.19.9"));
    assert!(kernel_has_arm64_rseq_bug("aarch64", "7.0.12-linuxkit"));
    assert!(kernel_has_arm64_rseq_bug("aarch64", "7.1.0-rc1"));
    assert!(!kernel_has_arm64_rseq_bug("aarch64", "7.0.14"));
    assert!(!kernel_has_arm64_rseq_bug("aarch64", "7.1.0-rc2"));
    assert!(!kernel_has_arm64_rseq_bug("aarch64", "6.18.3"));
    assert!(!kernel_has_arm64_rseq_bug("x86_64", "7.0.12"));
}
