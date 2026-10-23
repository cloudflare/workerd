// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::collections::HashMap;
use std::collections::HashSet;

use super::*;

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
    assert!(!crate::Number::holds_gc_references());
    assert!(<v8::TracedGlobal<v8::Value>>::holds_gc_references());
    assert_forwards!(Option);
    assert_forwards!(Nullable);
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
