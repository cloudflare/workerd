// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `Local<Name>` — the `v8::Name` supertype shared by `String` and `Symbol`.

use jsg::v8;
use jsg::v8::Local;

// =============================================================================
// Helpers
// =============================================================================

fn str_to_name<'a>(lock: &mut jsg::Lock, s: &str) -> Local<'a, v8::Name> {
    let js_str = v8::String::new_from_str(lock, s).unwrap(lock);
    js_str.into()
}

// =============================================================================
// Identity hash — via Local<Name>
// =============================================================================

#[test]
fn name_get_identity_hash_string_is_nonzero() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let name = str_to_name(lock, "hash-me");
        assert_ne!(name.get_identity_hash(), 0);
        Ok(())
    });
}

#[test]
fn name_get_identity_hash_string_is_stable() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let name = str_to_name(lock, "stable-hash");
        assert_eq!(name.get_identity_hash(), name.get_identity_hash());
        Ok(())
    });
}

#[test]
fn name_get_identity_hash_symbol_is_nonzero() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let sym = v8::Symbol::new(lock, None);
        let name: Local<v8::Name> = sym.into();
        assert_ne!(name.get_identity_hash(), 0);
        Ok(())
    });
}

#[test]
fn name_get_identity_hash_symbol_is_stable() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let sym = v8::Symbol::new(lock, None);
        let name: Local<v8::Name> = sym.clone().into();
        let name2: Local<v8::Name> = sym.into();
        assert_eq!(name.get_identity_hash(), name2.get_identity_hash());
        Ok(())
    });
}

// =============================================================================
// Upcasting: String → Name and Symbol → Name
// =============================================================================

#[test]
fn string_upcast_to_name_is_name() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = v8::String::new_from_str(lock, "hello").unwrap(lock);
        assert!(s.is_string());
        assert!(s.is_name());
        let name: Local<v8::Name> = s.into();
        assert!(name.is_name());
        Ok(())
    });
}

#[test]
fn symbol_upcast_to_name_is_name() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let sym = v8::Symbol::new(lock, None);
        assert!(sym.is_symbol());
        assert!(sym.is_name());
        let name: Local<v8::Name> = sym.into();
        assert!(name.is_name());
        Ok(())
    });
}
