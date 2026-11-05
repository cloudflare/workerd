// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::BTreeSet;
use std::collections::HashMap;

use super::*;

/// A visitor for a trace that never reaches it.
fn unused_visitor() -> v8::GcVisitor {
    v8::GcVisitor {
        handle: v8::ffi::GcVisitor { ptr: 0 },
        mode: v8::VisitMode::Visit,
    }
}

/// `HoldsNoGcReferences` follows the contents. (That a `jsg::Rc`, a `TracedGlobal` or a
/// `Lenient` does not qualify is checked by the `gc-reference-in-std-cell-is-not-traced`
/// compile test.)
#[test]
fn holds_no_gc_references_follows_the_contents() {
    fn holds_no_gc_references<T: HoldsNoGcReferences>() {}
    holds_no_gc_references::<String>();
    holds_no_gc_references::<Option<String>>();
    holds_no_gc_references::<Cell<Option<String>>>();
    holds_no_gc_references::<RefCell<Vec<String>>>();
    holds_no_gc_references::<HashMap<String, Nullable<String>>>();
    holds_no_gc_references::<BTreeSet<String>>();
    holds_no_gc_references::<crate::Cell<NonCoercible<String>>>();
    holds_no_gc_references::<crate::RefCell<Vec<String>>>();
}

#[test]
fn mutably_borrowed_refcell_without_gc_references_is_skipped() {
    let cell = RefCell::new(String::from("label"));
    let _borrow = cell.borrow_mut();
    cell.trace(&mut unused_visitor());
}
