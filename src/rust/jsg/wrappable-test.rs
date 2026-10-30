// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::collections::HashMap;
use std::collections::HashSet;
use std::panic::AssertUnwindSafe;

use super::*;

/// A visitor for a trace that never reaches it.
fn unused_visitor() -> v8::GcVisitor {
    v8::GcVisitor {
        handle: v8::ffi::GcVisitor { ptr: 0 },
    }
}

/// Asserts that wrapper `W` holds GC references exactly when what it wraps does.
macro_rules! assert_forwards {
    ($($w:ident)::+) => {
        assert!(<$($w)::+<v8::TracedGlobal<v8::Value>>>::holds_gc_references());
        assert!(!<$($w)::+<String>>::holds_gc_references());
    };
}

#[test]
fn holds_gc_references_follows_the_contents() {
    assert!(!String::holds_gc_references());
    assert!(!f64::holds_gc_references());
    assert!(<v8::TracedGlobal<v8::Value>>::holds_gc_references());
    assert_forwards!(Option);
    assert_forwards!(Nullable);
    assert_forwards!(NullOrUndefined);
    assert_forwards!(NonCoercible);
    assert_forwards!(Vec);
    assert_forwards!(HashSet);
    assert_forwards!(BTreeSet);
    assert_forwards!(Cell);
    assert_forwards!(RefCell);
    assert_forwards!(std::rc::Rc);
    assert!(<HashMap<String, v8::TracedGlobal<v8::Value>>>::holds_gc_references());
    assert!(!<HashMap<String, String>>::holds_gc_references());
    assert!(<BTreeMap<String, v8::TracedGlobal<v8::Value>>>::holds_gc_references());
    assert!(!<BTreeMap<String, String>>::holds_gc_references());
    // An unconvertable value is a `TracedGlobal`.
    assert!(<Lenient<String>>::holds_gc_references());
}

#[test]
fn mutably_borrowed_refcell_without_gc_references_is_skipped() {
    let cell = RefCell::new(String::from("label"));
    let _borrow = cell.borrow_mut();
    cell.trace(&mut unused_visitor());
}

#[test]
#[cfg_attr(
    not(debug_assertions),
    ignore = "the borrow check is a debug assertion"
)]
fn mutably_borrowed_refcell_with_gc_references_panics_in_debug_builds() {
    let cell: RefCell<Vec<v8::TracedGlobal<v8::Value>>> = RefCell::new(Vec::new());
    let _borrow = cell.borrow_mut();
    let payload = std::panic::catch_unwind(AssertUnwindSafe(|| {
        cell.trace(&mut unused_visitor());
    }))
    .expect_err("tracing a mutably borrowed RefCell of TracedGlobals must panic");
    assert!(
        crate::panic_message(&*payload).starts_with("tracing found a mutably borrowed RefCell")
    );
}
