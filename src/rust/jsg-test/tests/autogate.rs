// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for the Rust `autogate` crate, exercised via the `jsg` re-exports.
//!
//! The global autogate is not initialized in the test harness, so all gates
//! fall back to the `WORKERD_ALL_AUTOGATES` env-var check. Without that
//! variable set, every gate returns `false`.

use jsg::Autogate;
use jsg::AutogateKey;

const ALL_KEYS: &[AutogateKey] = &[
    AutogateKey::TestWorkerd,
    AutogateKey::V8FastApi,
    AutogateKey::StreamingTailWorker,
    AutogateKey::TailStreamRefactor,
    AutogateKey::RustBackedNodeDns,
    AutogateKey::RpcUseExternalPusher,
    AutogateKey::WasmShutdownSignalShim,
    AutogateKey::EnableFastTextencoder,
    AutogateKey::EnableDrainingReadOnStandardStreams,
];

/// Without the env-var or a config, all known gates return false.
#[test]
fn all_known_gates_return_false_by_default() {
    for &key in ALL_KEYS {
        assert!(
            !Autogate::is_enabled(key),
            "expected gate {key:?} to be disabled"
        );
    }
}

/// ALL_KEYS must list every variant — catches a gate added to C++ but not here.
/// The static_assert in ffi.h catches the reverse (ffi.rs out of date).
#[test]
fn all_keys_covers_all_variants() {
    // AutogateKey is #[repr(C)] with contiguous discriminants starting at 0.
    // The highest discriminant + 1 must equal the number of entries in ALL_KEYS.
    let max = ALL_KEYS.iter().map(|k| *k as u32).max().unwrap();
    assert_eq!(
        max + 1,
        ALL_KEYS.len() as u32,
        "ALL_KEYS has gaps or duplicates"
    );
}

/// Discriminants are contiguous from 0 — catches reordering in C++.
#[test]
fn key_discriminants_are_contiguous() {
    for (i, &key) in ALL_KEYS.iter().enumerate() {
        assert_eq!(
            key as u32, i as u32,
            "key {key:?} has unexpected discriminant"
        );
    }
}

/// `AutogateKey` is a zero-overhead enum — same size as its repr.
#[test]
fn autogate_key_is_int_sized() {
    assert_eq!(
        std::mem::size_of::<AutogateKey>(),
        std::mem::size_of::<i32>()
    );
}

/// `Autogate` itself carries no state.
#[test]
fn autogate_struct_is_zero_sized() {
    assert_eq!(std::mem::size_of::<Autogate>(), 0);
}
