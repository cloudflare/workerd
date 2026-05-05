// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `Local<Symbol>` — `v8::Symbol` bindings.

use jsg::v8;
use jsg::v8::Local;

// =============================================================================
// is_symbol on Local<Value>
// =============================================================================

#[test]
fn value_is_symbol_for_symbol() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("Symbol('test')").unwrap();
        assert!(val.is_symbol());
        Ok(())
    });
}

#[test]
fn value_is_symbol_false_for_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("'not a symbol'").unwrap();
        assert!(!val.is_symbol());
        Ok(())
    });
}

#[test]
fn value_is_symbol_false_for_number() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("42").unwrap();
        assert!(!val.is_symbol());
        Ok(())
    });
}

// =============================================================================
// Symbol::new — uniqueness and description
// =============================================================================

#[test]
fn symbol_new_without_description() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let sym = v8::Symbol::new(lock, None);
        assert!(sym.is_symbol());
        assert!(sym.description(lock).is_none());
        Ok(())
    });
}

#[test]
fn symbol_new_with_description() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let desc = v8::String::new_from_str(lock, "my-symbol").unwrap(lock);
        let sym = v8::Symbol::new(lock, Some(desc));
        assert!(sym.is_symbol());
        let got = sym.description(lock).expect("expected a description");
        assert_eq!(got.to_string(lock), "my-symbol");
        Ok(())
    });
}

#[test]
fn symbol_new_is_unique() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let a = v8::Symbol::new(lock, None);
        let b = v8::Symbol::new(lock, None);
        // Two different symbols are not the same V8 object.
        let a_val: Local<v8::Value> = a.into();
        let b_val: Local<v8::Value> = b.into();
        assert_ne!(a_val, b_val);
        Ok(())
    });
}

#[test]
fn symbol_with_same_description_is_still_unique() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let make_sym = |lock: &mut jsg::Lock| {
            let desc = v8::String::new_from_str(lock, "same").unwrap(lock);
            v8::Symbol::new(lock, Some(desc))
        };
        let a = make_sym(lock);
        let b = make_sym(lock);
        let a_val: Local<v8::Value> = a.into();
        let b_val: Local<v8::Value> = b.into();
        // Symbol('same') !== Symbol('same')
        assert_ne!(a_val, b_val);
        Ok(())
    });
}

// =============================================================================
// try_as cast from Local<Value>
// =============================================================================

#[test]
fn value_try_as_symbol_succeeds() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("Symbol('cast-test')").unwrap();
        assert!(val.is_symbol());
        let sym = val.try_as::<v8::Symbol>().expect("should cast to Symbol");
        assert!(sym.is_symbol());
        Ok(())
    });
}

#[test]
fn value_try_as_symbol_fails_for_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("'hello'").unwrap();
        assert!(val.try_as::<v8::Symbol>().is_none());
        Ok(())
    });
}

// =============================================================================
// Symbol → Value upcast
// =============================================================================

#[test]
fn symbol_into_value_preserves_is_symbol() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let sym = v8::Symbol::new(lock, None);
        let val: Local<v8::Value> = sym.into();
        assert!(val.is_symbol());
        assert!(!val.is_string());
        Ok(())
    });
}
